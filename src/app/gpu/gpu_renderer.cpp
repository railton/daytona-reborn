#include "app/gpu/gpu_renderer.h"

#include "app/gpu/m2_polygon_material.h"
#include "app/gpu/shaders_gen.h"
#include "app/gpu/dxr_shaders_gen.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace app {

namespace {

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }

constexpr uint32_t kSheetWords = 0x80000;                 // one texture sheet (VideoMem tex0, tex1)
constexpr uint32_t kTexramBytes = kSheetWords * 4 * 2;    // both sheets
constexpr uint32_t kLumaBytes = 0x20000;                  // luma RAM (one entry per dword)
constexpr uint32_t kXlatEntries = 0x6000;                 // colour translation (0xc000 bytes of u16)
constexpr uint32_t kTilePixBytes = 4 * 512 * 512 * 2;     // the four tilemap pixmaps, a u16 per pixel
constexpr uint32_t kSkyBytes = uint32_t(rt::Video::kSkyW) * rt::Video::kSkyH * 2; // the sky panorama, after them
constexpr uint32_t kTileHeader = 16;                      // m2.hlsl tiledata: header, pens, tile words
constexpr uint32_t kTileDataWords = kTileHeader + rt::Video::kGpuPens + rt::Video::kGpuTileWords / 2;

} // namespace

SDL_GPUShader *GpuRenderer::shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                                   const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms,
                                   int samplers, int storage_buffers) {
    SDL_GPUShaderCreateInfo si{};
    si.stage = stage;
    si.num_uniform_buffers = Uint32(uniforms);
    si.num_samplers = Uint32(samplers);
    si.num_storage_buffers = Uint32(storage_buffers);
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev_);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        si.format = SDL_GPU_SHADERFORMAT_SPIRV, si.code = spv, si.code_size = spv_len, si.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        si.format = SDL_GPU_SHADERFORMAT_DXIL, si.code = dxil, si.code_size = dxil_len, si.entrypoint = dxil_entry;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        si.format = SDL_GPU_SHADERFORMAT_MSL, si.code = reinterpret_cast<const Uint8 *>(msl),
        si.code_size = std::strlen(msl), si.entrypoint = "main0";
    } else {
        error_ = "no supported shader format (SPIR-V, DXIL or MSL)";
        return nullptr;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev_, &si);
    if (!s) error_ = std::string("SDL_CreateGPUShader: ") + SDL_GetError();
    return s;
}

void GpuRenderer::set_anisotropy(int n) {
    uint32_t bits = 0;
    while (bits < 4 && (2 << bits) <= n) ++bits;
    aniso_bits_ = bits;
}

bool GpuRenderer::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format, int msaa) {
    dev_ = dev;
    frame_format_ = format;
    for (int i = 0; i < 256; i++) // the rasterizer's gamma (MAME video_start)
        gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));

    SDL_GPUShader *vs_poly = shader(k_vs_poly_spv, sizeof k_vs_poly_spv, k_vs_poly_dxil, sizeof k_vs_poly_dxil,
                                    k_vs_poly_msl, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_poly = shader(k_ps_poly_spv, sizeof k_ps_poly_spv, k_ps_poly_dxil, sizeof k_ps_poly_dxil,
                                    k_ps_poly_msl, "ps_poly", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2, 6);
    SDL_GPUShader *vs_quad = shader(k_vs_quad_spv, sizeof k_vs_quad_spv, k_vs_quad_dxil, sizeof k_vs_quad_dxil,
                                    k_vs_quad_msl, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_quad = shader(k_ps_quad_spv, sizeof k_ps_quad_spv, k_ps_quad_dxil, sizeof k_ps_quad_dxil,
                                    k_ps_quad_msl, "ps_quad", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    SDL_GPUShader *ps_back = shader(k_ps_tiles_back_spv, sizeof k_ps_tiles_back_spv, k_ps_tiles_back_dxil,
                                    sizeof k_ps_tiles_back_dxil, k_ps_tiles_back_msl, "ps_tiles_back",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_front = shader(k_ps_tiles_front_spv, sizeof k_ps_tiles_front_spv, k_ps_tiles_front_dxil,
                                     sizeof k_ps_tiles_front_dxil, k_ps_tiles_front_msl, "ps_tiles_front",
                                     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_quad_smooth = shader(k_ps_quad_smooth_spv, sizeof k_ps_quad_smooth_spv, k_ps_quad_smooth_dxil,
                                           sizeof k_ps_quad_smooth_dxil, k_ps_quad_smooth_msl, "ps_quad_smooth",
                                           SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!vs_poly || !ps_poly || !vs_quad || !ps_quad || !ps_back || !ps_front || !ps_quad_smooth) {
        for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front, ps_quad_smooth})
            if (s) SDL_ReleaseGPUShader(dev_, s);
        return false;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = format;
    // MSAA: the most samples up to the request that the colour and depth formats take
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
    msaa_ = 1;
    for (const auto &[n, count] : {std::pair{8, SDL_GPU_SAMPLECOUNT_8}, std::pair{4, SDL_GPU_SAMPLECOUNT_4},
                                   std::pair{2, SDL_GPU_SAMPLECOUNT_2}})
        if (msaa >= n && SDL_GPUTextureSupportsSampleCount(dev, format, count) &&
            SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_D16_UNORM, count)) {
            samples = count;
            msaa_ = n;
            break;
        }
    samples_ = samples;
    // with MSAA the polygons leave the target's alpha alone (alpha to coverage writes its own)
    SDL_GPUColorTargetDescription poly_color = color;
    if (msaa_ > 1) {
        poly_color.blend_state.enable_color_write_mask = true;
        poly_color.blend_state.color_write_mask =
            SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    }

    // Polygons: position, depth (draw order), 1/z u/z v/z, colour, texture state.
    SDL_GPUVertexBufferDescription pvb{};
    pvb.slot = 0;
    pvb.pitch = sizeof(PolyVertex);
    pvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute pattr[9] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, ooz))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, Uint32(offsetof(PolyVertex, info))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, Uint32(offsetof(PolyVertex, more))},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, light_oz))},
        {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, normal_oz))},
        {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, position_oz))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_poly;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 9;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS; // earlier in the order wins
    pi.target_info.color_target_descriptions = &poly_color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = samples;
    pi.multisample_state.enable_alpha_to_coverage = msaa_ > 1; // cut-out textures' edges (m2.hlsl)
    poly_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    pi.target_info.color_target_descriptions = &color; // the shadow pipelines below
    pi.multisample_state = SDL_GPUMultisampleState{};

    // Layers: a textured quad; no depth test.
    SDL_GPUVertexBufferDescription qvb{};
    qvb.slot = 0;
    qvb.pitch = sizeof(QuadVertex);
    qvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.fragment_shader = ps_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the polygons
    qi.multisample_state.sample_count = samples;
    quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    // Tilemap layers: the same quad, composed per pixel.
    qi.fragment_shader = ps_back;
    back_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    // the front layers blend (smooth 2D: partly covered edge pixels); a pixel
    // of alpha 1 replaces the target's as before
    SDL_GPUColorTargetDescription front_color = color;
    front_color.blend_state.enable_blend = true;
    front_color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    front_color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    front_color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    front_color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    front_color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    front_color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    qi.target_info.color_target_descriptions = &front_color;
    qi.fragment_shader = ps_front;
    front_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    qi.fragment_shader = ps_quad_smooth; // the CPU's front layers, smooth (blending their edges too)
    quad_smooth_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    qi.target_info.color_target_descriptions = &color;

    for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front, ps_quad_smooth})
        SDL_ReleaseGPUShader(dev_, s);
    if (!poly_pipe_ || !quad_pipe_ || !back_pipe_ || !front_pipe_ || !quad_smooth_pipe_) {
        error_ = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo smp{}; // layers are drawn 1:1: nearest
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev_, &smp);
    shadow_sampler_ = SDL_CreateGPUSampler(dev_, &smp); // the shadow map: nearest, read as depth

    // The cars' shadow map: depth only, from the sun (set_car_shadows)
    constexpr SDL_GPUTextureUsageFlags kShadowUsage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadow_format_ = SDL_GPUTextureSupportsFormat(dev_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, kShadowUsage)
                         ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
                         : SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    {
        SDL_GPUTextureCreateInfo st{};
        st.type = SDL_GPU_TEXTURETYPE_2D;
        st.format = shadow_format_;
        st.usage = kShadowUsage;
        st.width = st.height = 1;
        st.layer_count_or_depth = st.num_levels = 1;
        shadow_none_ = SDL_CreateGPUTexture(dev_, &st);
        SDL_GPUShader *vs_shadow = shader(k_vs_shadow_spv, sizeof k_vs_shadow_spv, k_vs_shadow_dxil, sizeof k_vs_shadow_dxil,
                                          k_vs_shadow_msl, "vs_shadow", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader *ps_shadow = shader(k_ps_shadow_spv, sizeof k_ps_shadow_spv, k_ps_shadow_dxil, sizeof k_ps_shadow_dxil,
                                          k_ps_shadow_msl, "ps_shadow", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
        if (vs_shadow && ps_shadow) {
            SDL_GPUVertexAttribute sattr[2] = {
                {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
                {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
            };
            SDL_GPUGraphicsPipelineCreateInfo si{};
            si.vertex_shader = vs_shadow;
            si.fragment_shader = ps_shadow;
            si.vertex_input_state.vertex_buffer_descriptions = &pvb;
            si.vertex_input_state.num_vertex_buffers = 1;
            si.vertex_input_state.vertex_attributes = sattr;
            si.vertex_input_state.num_vertex_attributes = 2;
            si.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            si.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            si.depth_stencil_state.enable_depth_test = true;
            si.depth_stencil_state.enable_depth_write = true;
            si.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
            si.target_info.depth_stencil_format = shadow_format_;
            si.target_info.has_depth_stencil_target = true;
            shadow_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &si);
        }
        for (SDL_GPUShader *s : {vs_shadow, ps_shadow})
            if (s) SDL_ReleaseGPUShader(dev_, s);
        if (!shadow_pipe_) std::fprintf(stderr, "daytona: the cars' shadows unavailable: %s\n", SDL_GetError());
    }

    // The real sky the paint reflects (set_car_shading): the back layers
    // into a small texture, blurred by its mip levels
    {
        SDL_GPUShader *vs_post = shader(k_vs_post_spv, sizeof k_vs_post_spv, k_vs_post_dxil, sizeof k_vs_post_dxil,
                                        k_vs_post_msl, "vs_post", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader *ps_sky = shader(k_ps_sky_env_spv, sizeof k_ps_sky_env_spv, k_ps_sky_env_dxil,
                                       sizeof k_ps_sky_env_dxil, k_ps_sky_env_msl, "ps_sky_env",
                                       SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
        if (vs_post && ps_sky) {
            SDL_GPUColorTargetDescription sky_color{};
            sky_color.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            SDL_GPUGraphicsPipelineCreateInfo ki{};
            ki.vertex_shader = vs_post;
            ki.fragment_shader = ps_sky;
            ki.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            ki.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            ki.target_info.color_target_descriptions = &sky_color;
            ki.target_info.num_color_targets = 1;
            sky_env_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &ki);
        }
        for (SDL_GPUShader *sh : {vs_post, ps_sky})
            if (sh) SDL_ReleaseGPUShader(dev_, sh);
        SDL_GPUTextureCreateInfo ni{};
        ni.type = SDL_GPU_TEXTURETYPE_2D;
        ni.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ni.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ni.width = ni.height = 1;
        ni.layer_count_or_depth = ni.num_levels = 1;
        sky_none_ = SDL_CreateGPUTexture(dev_, &ni);
        SDL_GPUSamplerCreateInfo ks{};
        ks.min_filter = ks.mag_filter = SDL_GPU_FILTER_LINEAR;
        ks.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        ks.address_mode_u = ks.address_mode_v = ks.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        ks.max_lod = float(kSkyEnvLevels);
        sky_sampler_ = SDL_CreateGPUSampler(dev_, &ks);
        if (!sky_env_pipe_) std::fprintf(stderr, "daytona: the real sky in the reflections unavailable: %s\n", SDL_GetError());
    }
    if (!sky_none_ || !sky_sampler_) {
        error_ = std::string("SDL_GPU setup: ") + SDL_GetError();
        shutdown();
        return false;
    }

    SDL_GPUBufferCreateInfo qb{};
    qb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    qb.size = sizeof(QuadVertex) * 6;
    qbuf_ = SDL_CreateGPUBuffer(dev_, &qb);
    SDL_GPUBufferCreateInfo sb{};
    sb.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    sb.size = kTexramBytes;
    texram_ = SDL_CreateGPUBuffer(dev_, &sb);
    // local car bodies (experimental): the opponents' (M2_OPPONENT_BODIES)
    // and the player's (M2_PLAYER_BODY, its atlas after the opponents')
    if (const char *directory = std::getenv("M2_OPPONENT_BODIES"); directory && *directory) {
        have_opponents_ = opponent_bodies_.load(directory);
        std::fprintf(stderr, "daytona: local opponent bodies %s\n",
                     have_opponents_ ? "loaded" : "unavailable, keeping original bodies");
    }
    const char *player_env = std::getenv("M2_PLAYER_BODY");
    const std::string player_dir = player_env && *player_env ? std::string(player_env) : player_body_dir_;
    have_player_ = false;
    player_body_ = OpponentBodies{};
    if (!player_dir.empty()) {
        const std::string directory = player_dir;
        have_player_ = player_body_.load(directory, 32768);
        if (have_player_) player_body_.set_player(OpponentBodies::kPlayerTextureBase); // after the opponents' 3 x 10
        std::fprintf(stderr, "daytona: local player body %s\n",
                     have_player_ ? "loaded" : "unavailable, keeping the original car");
    }
    if (have_opponents_ || have_player_) {
        atlas_cpu_ = have_opponents_ ? opponent_bodies_.atlas() : std::vector<uint8_t>(OpponentBodies::atlas_bytes, 0);
        if (have_player_) atlas_cpu_.insert(atlas_cpu_.end(), player_body_.atlas().begin(), player_body_.atlas().end());
        sb.size = uint32_t(atlas_cpu_.size());
        opponent_atlas_ = SDL_CreateGPUBuffer(dev_, &sb);
        if (!opponent_atlas_) have_opponents_ = have_player_ = false;
    }
    sb.size = kLumaBytes;
    luma_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kXlatEntries * 4;
    xlat_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTilePixBytes + kSkyBytes;
    tilepix_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTileDataWords * 4;
    tiledata_ = SDL_CreateGPUBuffer(dev_, &sb);
    if (!sampler_ || !qbuf_ || !texram_ || !luma_ || !xlat_ || !tilepix_ || !tiledata_) {
        error_ = std::string("SDL_GPU setup: ") + SDL_GetError();
        shutdown();
        return false;
    }
    return true;
}

void GpuRenderer::shutdown() {
    if (!dev_) return;
    if (poly_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, poly_pipe_), poly_pipe_ = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&quad_pipe_, &back_pipe_, &front_pipe_, &quad_smooth_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_), sampler_ = nullptr;
    if (shadow_sampler_) SDL_ReleaseGPUSampler(dev_, shadow_sampler_), shadow_sampler_ = nullptr;
    if (shadow_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, shadow_pipe_), shadow_pipe_ = nullptr;
    for (SDL_GPUTexture **t : {&shadow_map_, &shadow_none_})
        if (*t) SDL_ReleaseGPUTexture(dev_, *t), *t = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&z_pipe_, &z_front_pipe_, &z_quad_pipe_, &shade_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    if (view_z_) SDL_ReleaseGPUTexture(dev_, view_z_), view_z_ = nullptr;
    if (light_) SDL_ReleaseGPUTexture(dev_, light_), light_ = nullptr;
    shadows_ = shadow_ready_ = false;
    shadow_frame_ = ShadowFrame{};
    if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
    for (SDL_GPUTexture **t : {&msaa_color_, &resolve_})
        if (*t) SDL_ReleaseGPUTexture(dev_, *t), *t = nullptr;
    msaa_w_ = msaa_h_ = 0;
    for (SDL_GPUBuffer **b : {&hires_index_, &hires_data_})
        if (*b) SDL_ReleaseGPUBuffer(dev_, *b), *b = nullptr;
    hires_data_uploaded_ = false;
    hires_generation_ = ~0ull;
    hires_index_ready_ = false;
    hires_indexer_.reset();
    if (sky_env_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, sky_env_pipe_), sky_env_pipe_ = nullptr;
    for (SDL_GPUTexture **t : {&sky_env_, &sky_none_})
        if (*t) SDL_ReleaseGPUTexture(dev_, *t), *t = nullptr;
    if (sky_sampler_) SDL_ReleaseGPUSampler(dev_, sky_sampler_), sky_sampler_ = nullptr;
    for (SDL_GPUShader **sh : {&post_vs_, &post_ps_})
        if (*sh) SDL_ReleaseGPUShader(dev_, *sh), *sh = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&post_bright_pipe_, &post_blur_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    for (auto &fp : post_pipes_) SDL_ReleaseGPUGraphicsPipeline(dev_, fp.second);
    post_pipes_.clear();
    if (post_sampler_) SDL_ReleaseGPUSampler(dev_, post_sampler_), post_sampler_ = nullptr;
    for (SDL_GPUTexture *&t : glow_)
        if (t) SDL_ReleaseGPUTexture(dev_, t), t = nullptr;
    glow_w_ = glow_h_ = 0;
    post_failed_ = false;
    if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_), vbuf_ = nullptr;
    if (qbuf_) SDL_ReleaseGPUBuffer(dev_, qbuf_), qbuf_ = nullptr;
    for (SDL_GPUBuffer **b : {&texram_, &luma_, &xlat_, &tilepix_, &tiledata_, &opponent_atlas_})
        if (*b) SDL_ReleaseGPUBuffer(dev_, *b), *b = nullptr;
    texram_generation_ = ~0ull;
    opponent_atlas_uploaded_ = false;
    tile_instance_ = 0;
    if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_), upload_ = nullptr;
    vbuf_size_ = upload_size_ = 0;
    depth_w_ = depth_h_ = front_w_ = front_h_ = 0;
}

// Textures and buffers big enough for this frame.
bool GpuRenderer::ensure(int w, int h, int pixel_w, int pixel_h, uint32_t vert_bytes) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    if (w != front_w_ || h != front_h_) {
        if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
        ti.width = Uint32(w);
        ti.height = Uint32(h);
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the layers' 0xAARRGGBB words
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        front_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!front_) return false;
        front_w_ = w, front_h_ = h;
    }
    if (pixel_w != depth_w_ || pixel_h != depth_h_) {
        if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
        ti.width = Uint32(pixel_w);
        ti.height = Uint32(pixel_h);
        ti.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        ti.sample_count = samples_;
        depth_ = SDL_CreateGPUTexture(dev_, &ti);
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        if (!depth_) return false;
        depth_w_ = pixel_w, depth_h_ = pixel_h;
    }
    if (msaa_ > 1 && (pixel_w != msaa_w_ || pixel_h != msaa_h_)) {
        for (SDL_GPUTexture **t : {&msaa_color_, &resolve_})
            if (*t) SDL_ReleaseGPUTexture(dev_, *t), *t = nullptr;
        ti.width = Uint32(pixel_w);
        ti.height = Uint32(pixel_h);
        ti.format = frame_format_;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.sample_count = samples_;
        msaa_color_ = SDL_CreateGPUTexture(dev_, &ti);
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        resolve_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!msaa_color_ || !resolve_) return false;
        msaa_w_ = pixel_w, msaa_h_ = pixel_h;
    }
    if (vert_bytes > vbuf_size_) {
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        vbuf_size_ = std::max<uint32_t>(vert_bytes, 1u << 20) * 2;
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vbuf_size_;
        vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
        if (!vbuf_) return false;
    }
    const uint32_t need = uint32_t(w) * uint32_t(h) * 4 + uint32_t(sizeof(QuadVertex) * 6) + vert_bytes +
                          kTexramBytes + kLumaBytes + kXlatEntries * 4 + kTilePixBytes + kSkyBytes + kTileDataWords * 4 + (opponent_atlas_ && !opponent_atlas_uploaded_ ? uint32_t(atlas_cpu_.size()) : 0);
    if (need > upload_size_) {
        if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_);
        upload_size_ = need * 2;
        SDL_GPUTransferBufferCreateInfo tb{};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = upload_size_;
        upload_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
        if (!upload_) return false;
    }
    return true;
}

// The polygons as triangles, in the rasterizer's order, batched by clip rectangle.
// The cars' shading (set_car_shading). Each face of a car (a keyed mobile:
// rt::MobileIdentity) gets its normal in camera space (Newell's, turned to
// the camera) from its corners (x and y divided by the focal length, z);
// its corners are matched by their object-space position within the same
// object emission. Each corner then takes, over the faces sharing it whose
// normal is within 50 degrees of its own face's (a crease stays sharp), the
// area-weighted mean of their normals and of the light the game gave them
// (GeoPoly::luma: the game's own lighting, only spread across the faces).
void GpuRenderer::shade_cars(const std::vector<rt::GeoPoly> &polys) {
    car_first_.clear();
    car_corners_.clear();
    if (!smooth_cars_ && !car_reflect_ && !sharp_car_textures_ && !car_shadows_) return;
    car_first_.assign(polys.size(), -1);
    struct Face { float n[3], area, light; };
    std::vector<Face> faces(polys.size());
    // each corner as (emission, object-space position), sorted: the corners
    // sharing a position are then side by side (a hash of vectors, one per
    // corner, cost 5 ms a frame with a 20,000-face player's car)
    using Corner = ShadeCorner;
    auto same = [](const Corner &a, const Corner &b) {
        return a.command == b.command && a.x == b.x && a.y == b.y && a.z == b.z;
    };
    std::vector<Corner> &corners = corner_scratch_;
    corners.clear();
    uint32_t total = 0;
    // the player's custom body brings its corners, made once from its shape
    const auto *made = have_player_ && &polys == &player_body_.scene() ? &player_body_.corners() : nullptr;
    std::vector<size_t> &made_polys = made_scratch_;
    made_polys.clear();
    for (size_t i = 0; i < polys.size(); ++i) {
        const rt::GeoPoly &p = polys[i];
        // (the opponents' local bodies are not lit; the player's are: their
        // textures come after the opponents', OpponentBodies::set_player)
        const bool opponent_local = p.local_texture && p.local_texture <= OpponentBodies::kPlayerTextureBase;
        if (!p.mobile_key.part || p.num_vertices < 3 || opponent_local || rt::Geo::hud_overlay_address(p.object_oba) ||
            p.local_texture == OpponentBodies::kShadowOnlyTexture)
            continue;
        float pos[8][3], c[3] = {0, 0, 0}, n[3] = {0, 0, 0};
        for (int k = 0; k < p.num_vertices; ++k) {
            pos[k][0] = p.v[k].x / p.object_focus_x;
            pos[k][1] = p.v[k].y / p.object_focus_y;
            pos[k][2] = p.v[k].p[0];
            for (int a = 0; a < 3; ++a) c[a] += pos[k][a];
        }
        for (int k = 0; k < p.num_vertices; ++k) { // Newell
            const float *a = pos[k], *b = pos[(k + 1) % p.num_vertices];
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(len > 1e-12f)) continue;
        const float turn = n[0] * c[0] + n[1] * c[1] + n[2] * c[2] > 0 ? -1.0f : 1.0f; // towards the camera
        faces[i] = {{n[0] / len * turn, n[1] / len * turn, n[2] / len * turn}, len * 0.5f, float(p.luma)};
        car_first_[i] = int32_t(total);
        total += p.num_vertices;
        if (made && i < made->size() && (*made)[i].valid) {
            made_polys.push_back(i);
            continue;
        }
        for (int k = 0; k < p.num_vertices; ++k) {
            Corner e{p.object_command, 0, 0, 0, uint32_t(i), uint32_t(k)};
            std::memcpy(&e.x, &p.v[k].ox, 4);
            std::memcpy(&e.y, &p.v[k].oy, 4);
            std::memcpy(&e.z, &p.v[k].oz, 4);
            corners.push_back(e);
        }
    }
    car_corners_.resize(total);
    for (size_t i : made_polys) {
        const auto &m = (*made)[i];
        for (int k = 0; k < polys[i].num_vertices; ++k)
            car_corners_[size_t(car_first_[i]) + k] = {m.light[k], {m.normal[k][0], m.normal[k][1], m.normal[k][2]}};
    }
    std::sort(corners.begin(), corners.end(), [](const Corner &a, const Corner &b) {
        // then in the scene's order, as the faces were summed before: the same light to the bit
        return std::tie(a.command, a.x, a.y, a.z, a.poly, a.k) < std::tie(b.command, b.x, b.y, b.z, b.poly, b.k);
    });
    constexpr float kCrease = 0.6428f; // cos 50 degrees
    for (size_t g = 0; g < corners.size();) {
        size_t end = g + 1;
        while (end < corners.size() && same(corners[end], corners[g])) ++end;
        for (size_t e = g; e < end; ++e) {
            const Face &self = faces[corners[e].poly];
            float n[3] = {0, 0, 0}, light = 0, weight = 0;
            for (size_t o = g; o < end; ++o) {
                const Face &f = faces[corners[o].poly];
                if (f.n[0] * self.n[0] + f.n[1] * self.n[1] + f.n[2] * self.n[2] < kCrease) continue;
                for (int a = 0; a < 3; ++a) n[a] += f.n[a] * f.area;
                light += f.light * f.area;
                weight += f.area;
            }
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            CarCorner corner{self.light, {self.n[0], self.n[1], self.n[2]}};
            if (weight > 0 && len > 1e-12f) {
                corner.light = light / weight;
                for (int a = 0; a < 3; ++a) corner.normal[a] = n[a] / len;
            }
            car_corners_[size_t(car_first_[corners[e].poly]) + corners[e].k] = corner;
        }
        g = end;
    }
}

// The cars' shadow map (set_car_shadows): the sun's view, axes from the
// game's light vector (camera space), in two cascades side by side in one
// depth texture (kShadowMapW x kShadowMapH, each half square): the casters
// (build: the cars' opaque faces) within kShadowNear of the camera in the
// left half, those up to kShadowFar in the right, each fitted to its own
// casters with a margin, so that the near cars keep their detail however far
// the others are. Their triangles go after the scene's in verts_, already in
// their half's clip space. The fragment uniform (car_light_) tells ps_poly the
// same views; its strength is 0 when nothing casts.
void GpuRenderer::shadow_casters(const std::vector<rt::GeoPoly> &polys) {
    shadow_first_ = uint32_t(verts_.size());
    shadow_near_ = shadow_far_ = 0;
    std::fill(std::begin(car_light_) + 4, std::end(car_light_), 0.0f);
    if (casters_now_.empty()) return;
    constexpr float kShadowNear = 40.0f;  // camera units: the player's car is about 5 away
    constexpr float kShadowFar = 140.0f;  // the opponents ahead, at a coarser scale
    constexpr float kShadowDepth = 60.0f; // how far beyond the casters a receiver may be, along the light
    constexpr float kStrength = 0.45f;    // the light taken away in shadow
    float dir[3] = {game_light_[0], game_light_[1], game_light_[2]}; // the light's travel
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-6f)) return;
    for (float &c : dir) c /= len;
    const float hint[3] = {0.0f, std::fabs(dir[1]) > 0.95f ? 0.0f : 1.0f, std::fabs(dir[1]) > 0.95f ? 1.0f : 0.0f};
    float right[3] = {hint[1] * dir[2] - hint[2] * dir[1], hint[2] * dir[0] - hint[0] * dir[2], hint[0] * dir[1] - hint[1] * dir[0]};
    len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float &c : right) c /= len;
    const float up[3] = {dir[1] * right[2] - dir[2] * right[1], dir[2] * right[0] - dir[0] * right[2], dir[0] * right[1] - dir[1] * right[0]};
    auto view = [&](const rt::GeoPoly &p, int k, float out[3]) {
        const float x = p.v[k].x / p.object_focus_x, y = p.v[k].y / p.object_focus_y, z = p.v[k].p[0];
        out[0] = x * right[0] + y * right[1] + z * right[2];
        out[1] = x * up[0] + y * up[1] + z * up[2];
        out[2] = x * dir[0] + y * dir[1] + z * dir[2];
    };
    std::vector<size_t> kept[2]; // near, far
    float lo[2][3], hi[2][3];
    for (int c = 0; c < 2; ++c)
        for (int a = 0; a < 3; ++a) lo[c][a] = 1e30f, hi[c][a] = -1e30f;
    // a whole car goes to one cascade, by its farthest corner: assigned by
    // face, a car crossing kShadowNear was split between the two maps, its
    // shadow's halves at two sharpnesses
    std::unordered_map<uint32_t, float> car_far; // mobile slot -> farthest depth of its casting faces
    for (size_t i : casters_now_) {
        const rt::GeoPoly &p = polys[i];
        float &far = car_far[p.mobile_key.slot];
        for (int k = 0; k < p.num_vertices; ++k) far = std::max(far, p.v[k].p[0]);
    }
    for (size_t i : casters_now_) {
        const rt::GeoPoly &p = polys[i];
        if (p.num_vertices < 3) continue;
        float zmin = 1e30f, zmax = 0.0f;
        for (int k = 0; k < p.num_vertices; ++k) zmin = std::min(zmin, p.v[k].p[0]), zmax = std::max(zmax, p.v[k].p[0]);
        if (zmin <= 0.0f || zmax >= kShadowFar) continue;
        const int c = car_far[p.mobile_key.slot] < kShadowNear ? 0 : 1;
        kept[c].push_back(i);
        for (int k = 0; k < p.num_vertices; ++k) {
            float l[3];
            view(p, k, l);
            for (int a = 0; a < 3; ++a) lo[c][a] = std::min(lo[c][a], l[a]), hi[c][a] = std::max(hi[c][a], l[a]);
        }
    }
    if (kept[0].empty() && kept[1].empty()) return;
    float *u = car_light_;
    std::copy(right, right + 3, u + 4);
    std::copy(up, up + 3, u + 8);
    std::copy(dir, dir + 3, u + 12);
    u[22] = kStrength, u[23] = 1.0f / float(kShadowMapH); // a texel of a half
    for (int c = 0; c < 2; ++c) {
        if (kept[c].empty()) continue;
        const float pad = c ? 1.0f : 0.5f;
        lo[c][0] -= pad, lo[c][1] -= pad, hi[c][0] += pad, hi[c][1] += pad, lo[c][2] -= pad;
        const float range = hi[c][2] - lo[c][2] + kShadowDepth;
        const size_t before = verts_.size();
        for (size_t i : kept[c]) {
            const rt::GeoPoly &p = polys[i];
            PolyVertex v[8] = {};
            for (int k = 0; k < p.num_vertices; ++k) {
                float l[3];
                view(p, k, l);
                v[k].x = (l[0] - lo[c][0]) / (hi[c][0] - lo[c][0]) * 2.0f - 1.0f;
                v[k].y = (l[1] - lo[c][1]) / (hi[c][1] - lo[c][1]) * 2.0f - 1.0f;
                v[k].depth = (l[2] - lo[c][2]) / range;
            }
            for (int k = 1; k + 1 < p.num_vertices; ++k) {
                verts_.push_back(v[0]);
                verts_.push_back(v[k]);
                verts_.push_back(v[k + 1]);
            }
        }
        (c ? shadow_far_ : shadow_near_) = uint32_t(verts_.size() - before);
        float *m = c ? u + 28 : u + 16; // CarLight map / map2
        m[0] = lo[c][0], m[1] = lo[c][1], m[2] = 1.0f / (hi[c][0] - lo[c][0]), m[3] = 1.0f / (hi[c][1] - lo[c][1]);
        float *d = c ? u + 32 : u + 20; // depth / depth2 (x, y)
        d[0] = lo[c][2], d[1] = 1.0f / range;
    }
}

void GpuRenderer::build(const rt::Video &video, int w, int h, int scale, bool sharp_textures,
                        const std::vector<rt::GeoPoly> &polys) {
    verts_.clear();
    batches_.clear();
    if (triangles_) triangles_->clear();
    cameras_.clear();
    shadow_polys_.clear();
    z_draws_.clear();
    cutout_xyz_.clear();
    cutout_records_.clear();
    main_camera_ = ~0u;
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    // widescreen: the screen is `margin` wider on each side; x shifts by it
    const int margin = (w - rt::Video::W) / 2;
    const int crtc_x = video.crtc_x() + margin, crtc_y = video.crtc_y(), render_x = video.render_x() + margin,
              render_y = video.render_y();
    // HUD at the edges: the condition panel's own quads (its z, inside its box) move
    const int hud_dx = video.gpu_hud_shift();
    const float *hud_box = video.raster().hud_box();
    const uint16_t hud_z = video.raster().hud_z();
    if (polys.empty() || !mem.palram || !mem.colorxlat) return;
    // internal resolution: the mip level the rasterizer picks from z, less
    // log2(scale) (its levels are 128 units of fast_log2)
    const int lod_bias = int(std::lround(128.0 * std::log2(double(scale))));

    // Raster::render: windows from the last down to 0, low z first, newest first within z.
    order_.resize(polys.size());
    std::iota(order_.begin(), order_.end(), size_t(0));
    std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    const float depth_step = 1.0f / float(order_.size() + 2);
    shade_cars(polys);
    casters_now_.clear();
    // the cars' shadows fall on what the cars' camera sees (not the minimap's)
    bool have_car_camera = false;
    std::array<float, 5> car_camera{};
    if (car_shadows_ && shadow_pipe_)
        for (size_t i = 0; i < polys.size() && !have_car_camera; ++i)
            if (!car_first_.empty() && car_first_[i] >= 0) {
                const rt::GeoPoly &p = polys[i];
                car_camera = {float(p.center[0]), float(p.center[1]), p.object_focus_x, p.object_focus_y, float(p.window)};
                have_car_camera = true;
            }
    // the real sky's reflection: the cars' camera, in the frame's uv
    std::fill(std::begin(sky_camera_), std::end(sky_camera_), 0.0f);
    if (car_reflect_)
        for (size_t i = 0; i < polys.size(); ++i)
            if (!car_first_.empty() && car_first_[i] >= 0) {
                const rt::GeoPoly &p = polys[i];
                sky_camera_[0] = float(crtc_x + p.center[0]) / float(w);
                sky_camera_[1] = float((384 - p.center[1]) + crtc_y) / float(h);
                sky_camera_[2] = p.object_focus_x / float(w);
                sky_camera_[3] = p.object_focus_y / float(h);
                break;
            }
    uint32_t drawn = 0;
    for (size_t i : order_) {
        const rt::GeoPoly &poly = polys[i];
        if (poly.window > windows || poly.num_vertices < 3) continue;
        if (poly.local_texture == OpponentBodies::kShadowOnlyTexture) { // a shadow caster only, never drawn
            if (have_car_camera && car_shadows_) casters_now_.push_back(i);
            continue;
        }
        const int renderer = (poly.texheader[0] >> 13) & 3;
        if (renderer == 1) continue; // translucent solid: the rasterizer draws nothing
        // the game's own shadow under the player's car (its flat checker part
        // 0x78b4), left out where the cars' shadows replace it
        if (have_car_camera && hide_game_shadow_ && !car_first_.empty() && car_first_[i] >= 0 &&
            poly.mobile_key.part == 0x78b4u && ((poly.texheader[0] >> 15) & 1u))
            continue;
        // Diagnostic (3D tree study): cut-out textured scenery polygons
        // (renderer 3, a ROM object, not a car part). M2_CUTOUT_TRACE=FILE logs
        // one line per such polygon (object, texture words, object-space
        // extent, view-space centre); M2_HIDE_CUTOUT=1 skips them, to see
        // what they are on screen.
        if (renderer == 3 && (poly.object_oba & 0x01800000u) == 0x00800000u && !poly.mobile_key.part) {
            static std::FILE *cut_trace = [] {
                const char *p = std::getenv("M2_CUTOUT_TRACE");
                return p && *p ? std::fopen(p, "w") : nullptr;
            }();
            static const bool hide_cut = [] {
                const char *v = std::getenv("M2_HIDE_CUTOUT");
                return v && v[0] == '1';
            }();
            if (cut_trace) {
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (int k = 0; k < poly.source_num_vertices; ++k)
                    for (int a = 0; a < 3; ++a)
                        lo[a] = std::min(lo[a], poly.source_v[k][a]), hi[a] = std::max(hi[a], poly.source_v[k][a]);
                float cx = 0, cy = 0, cz = 0;
                for (int k = 0; k < poly.num_vertices; ++k) cx += poly.v[k].x, cy += poly.v[k].y, cz += poly.v[k].p[0];
                const float nv = float(std::max<int>(poly.num_vertices, 1));
                std::fprintf(cut_trace, "%x,%04x,%04x,%04x,%04x,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.5f\n",
                             poly.object_oba, poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3],
                             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], cx / nv, cy / nv, cz / nv);
            }
            if (hide_cut) continue;
            // M2_HIDE_CUTOUT_TEX="w2:w3,w2:w3" (hex texture header words 2 and 3):
            // skip only those textures, to tell which ones are the trees.
            static const std::vector<uint32_t> hide_tex = [] {
                std::vector<uint32_t> keys;
                if (const char *v = std::getenv("M2_HIDE_CUTOUT_TEX"))
                    for (const char *p = v; *p;) {
                        char *end = nullptr;
                        const unsigned long w2 = std::strtoul(p, &end, 16);
                        if (!end || *end != ':') break;
                        const unsigned long w3 = std::strtoul(end + 1, &end, 16);
                        keys.push_back(uint32_t(w2 << 16 | (w3 & 0xffff)));
                        p = (*end == ',') ? end + 1 : end;
                    }
                return keys;
            }();
            if (!hide_tex.empty() &&
                std::find(hide_tex.begin(), hide_tex.end(), uint32_t(poly.texheader[2]) << 16 | poly.texheader[3]) != hide_tex.end())
                continue;
        }

        // HUD overlay quad? (Raster::render_one's test, on the projected outline)
        int dx = 0;
        if (hud_dx && poly.z == hud_z) {
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            for (int k = 0; k < poly.num_vertices; k++) {
                const rt::GeoVertex &g0 = poly.v[k];
                const float z = g0.p[0] + std::numeric_limits<float>::min();
                const float px = float(crtc_x + poly.center[0]) + g0.x / z - float(margin);
                const float py = float((384 - poly.center[1]) + crtc_y) - g0.y / z;
                bx0 = std::min(bx0, px), bx1 = std::max(bx1, px), by0 = std::min(by0, py), by1 = std::max(by1, py);
            }
            constexpr float kTol = 1.5f;
            if (bx0 >= hud_box[0] - kTol && bx1 <= hud_box[1] + kTol && by0 >= hud_box[2] - kTol && by1 <= hud_box[3] + kTol)
                dx = hud_dx;
        }
        // clip rectangle (Raster::render_one), in the renderer's offsets, within the screen;
        // a viewport spanning the screen extends into the widescreen margins
        const int wide = margin && poly.viewport[0] <= 0 && poly.viewport[2] >= 495 ? margin : 0;
        const int x0 = std::max(poly.viewport[0] - wide + render_x + dx, 0),
                  x1 = std::min(poly.viewport[2] + wide + render_x + dx, w - 1);
        const int y0 = std::max((384 - poly.viewport[3]) + render_y, 0), y1 = std::min((384 - poly.viewport[1]) + render_y, h - 1);
        if (x0 > x1 || y0 > y1) continue;
        const SDL_Rect clip{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        if (batches_.empty() || std::memcmp(&batches_.back().clip, &clip, sizeof clip) != 0)
            batches_.push_back({uint32_t(verts_.size()), 0, clip});

        // colour: the solid renderer's (palette entry, luma translation, gamma)
        const uint32_t luma = poly.luma >> 2;
        const uint16_t color = le16(mem.palram, m2gpu::polygon_palette_index(poly.texheader[3]));
        const float r = gamma_[le16(mem.colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float depth = float(++drawn) * depth_step;

        // texture state, decoded as render_one (m2.hlsl PolyIn)
        auto material = m2gpu::pack_polygon_material(
            poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.luma, poly.texlod, lod_bias, color);
        if (sharp_textures) material.info[0] |= 1u << 23;
        if (msaa_ > 1) material.info[0] |= 1u << 25; // alpha to coverage for cut-out texels (m2.hlsl)
        material.info[0] |= aniso_bits_ << 26;
        if (hires_data_uploaded_ && hires_index_ready_ && renderer >= 2) material.info[0] |= 1u << 11; // m2.hlsl hires_texel
        if (poly.local_texture) {
            material.info[0] = 1u << 24;
            material.info[1] = poly.local_texture - 1;
            material.info[2] = poly.luma;
        }

        // the cars' shading (shade_cars): bit 29 smooth light, bit 30 reflections
        const int32_t car = car_first_.empty() ? -1 : car_first_[i];
        bool receives = false; // the cars' shadows: on the scenery seen through the cars' camera
        if (have_car_camera && car < 0 && !poly.local_texture && !rt::Geo::hud_overlay_address(poly.object_oba)) {
            const std::array<float, 5> camera{float(poly.center[0]), float(poly.center[1]), poly.object_focus_x,
                                              poly.object_focus_y, float(poly.window)};
            receives = camera == car_camera;
            for (int k = 0; receives && k < poly.num_vertices; ++k) receives = poly.v[k].p[0] > 0.0f;
        }
        // a car's opaque faces near the camera cast them (not its see-through
        // ones: the game's own checker shadow under it, the windows)
        const bool opponent_local = poly.local_texture && poly.local_texture <= OpponentBodies::kPlayerTextureBase;
        if (have_car_camera && car >= 0 && !opponent_local && !((material.info[0] >> 2) & 1u) &&
            (material.info[0] & 3u) != 3u)
            casters_now_.push_back(i);
        if (car >= 0 && !opponent_local) {
            if (smooth_cars_) material.info[0] |= 1u << 29;
            if (sharp_car_textures_) material.info[0] |= 1u << 31;
            // the paint only: the bodies (the player's car 0x7d68 and its
            // 0x7d94, the opponents' models), not the wheels, shadows or lights
            const uint32_t part = poly.mobile_key.part;
            if (car_reflect_ && (rt::is_opponent_body_part(part) || part == 0x7d68u || part == 0x7d94u))
                material.info[0] |= 1u << 30;
        }

        // model2_3d_project, then a fan from vertex 0 (the polygon is convex)
        PolyVertex v[8];
        for (int k = 0; k < poly.num_vertices; k++) {
            const rt::GeoVertex &g0 = poly.v[k];
            const float z = g0.p[0] + std::numeric_limits<float>::min();
            const float ooz = 1.0f / z; // render_one, textured: p[0] = 1/z, p[1..2] scaled by it / 8
            v[k] = {float(crtc_x + dx + poly.center[0]) + g0.x / z, float((384 - poly.center[1]) + crtc_y) - g0.y / z, depth,
                    ooz, g0.p[1] * ooz * (1.0f / 8.0f), g0.p[2] * ooz * (1.0f / 8.0f),
                    r, g, b, 1.0f, {material.info[0], material.info[1], material.info[2], material.info[3]},
                    {material.more[0], material.more[1]}};
            if (car >= 0) {
                const CarCorner &c = car_corners_[size_t(car + k)];
                v[k].light_oz = c.light * ooz;
                for (int a = 0; a < 3; ++a) v[k].normal_oz[a] = c.normal[a] * ooz;
                v[k].position_oz[0] = g0.x / poly.object_focus_x * ooz;
                v[k].position_oz[1] = g0.y / poly.object_focus_y * ooz;
                v[k].position_oz[2] = -1.0f; // z * (1 / z); negative: a car, which receives no shadows
            } else if (receives) {
                v[k].position_oz[0] = g0.x / poly.object_focus_x * ooz;
                v[k].position_oz[1] = g0.y / poly.object_focus_y * ooz;
                v[k].position_oz[2] = 1.0f;
            }
        }
        if (triangles_) {
            // The shadows' camera is the most common one (window, centre,
            // focal length); polygons drawn through another (the 3D minimap
            // in its own window) neither cast nor receive shadows.
            bool in_front = true;
            for (int k = 0; k < poly.num_vertices; k++) in_front = in_front && poly.v[k].p[0] > 0.0f;
            uint32_t camera_index = ~0u;
            if (in_front && dx == 0 && !rt::Geo::hud_overlay_address(poly.object_oba)) {
                const std::array<float, 5> camera{float(crtc_x + poly.center[0]), float((384 - poly.center[1]) + crtc_y),
                                                  poly.object_focus_x, poly.object_focus_y, float(poly.window)};
                auto it = std::find_if(cameras_.begin(), cameras_.end(), [&](const auto &c) { return c.first == camera; });
                if (it == cameras_.end()) it = cameras_.insert(cameras_.end(), {camera, 0});
                ++it->second;
                camera_index = uint32_t(it - cameras_.begin());
            }
            // Cut-out textures (renderer 3: fir trees, fences) go apart, with
            // their texture coordinates, so a ray passes through the holes.
            const bool cutout = renderer == 3 && !poly.local_texture;
            std::vector<float> &xyz = cutout ? cutout_xyz_ : *triangles_;
            const uint32_t tri_first = uint32_t(xyz.size() / 9);
            for (int k = 1; camera_index != ~0u && k + 1 < poly.num_vertices; k++) {
                if (cutout) {
                    cutout_records_.insert(cutout_records_.end(), {material.info[0], material.info[1]});
                    for (int corner : {0, k, k + 1}) // u, v as m2.hlsl ps_poly reads them: p[1..2] / 8 * 256
                        for (float t : {poly.v[corner].p[1] * 32.0f, poly.v[corner].p[2] * 32.0f}) {
                            uint32_t bits;
                            std::memcpy(&bits, &t, 4);
                            cutout_records_.push_back(bits);
                        }
                }
                for (int corner : {0, k, k + 1}) {
                    const rt::GeoVertex &g = poly.v[corner];
                    xyz.insert(xyz.end(), {g.x, g.y, g.p[0]});
                }
            }
            shadow_polys_.push_back({uint32_t(verts_.size()), uint32_t(3 * (poly.num_vertices - 2)),
                                     uint32_t(batches_.size() - 1), camera_index, tri_first,
                                     uint32_t(xyz.size() / 9) - tri_first, cutout});
        }
        for (int k = 1; k + 1 < poly.num_vertices; k++) {
            verts_.push_back(v[0]);
            verts_.push_back(v[k]);
            verts_.push_back(v[k + 1]);
        }
        batches_.back().count = uint32_t(verts_.size()) - batches_.back().first;
    }
    shadow_casters(polys);
    if (!triangles_) return;
    // Keep the main camera's polygons: their triangles for the acceleration
    // structure, their vertex runs (per clip batch) for the view z pass.
    const auto main = std::max_element(cameras_.begin(), cameras_.end(),
                                       [](const auto &a, const auto &b) { return a.second < b.second; });
    main_camera_ = main == cameras_.end() ? ~0u : uint32_t(main - cameras_.begin());
    size_t kept = 0, kept_cut = 0; // in triangles
    for (const ShadowPoly &s : shadow_polys_) {
        if (s.camera != main_camera_) continue;
        if (s.cutout) {
            if (kept_cut != s.tri_first) {
                std::copy_n(cutout_xyz_.begin() + 9 * s.tri_first, 9 * s.tri_count, cutout_xyz_.begin() + 9 * kept_cut);
                std::copy_n(cutout_records_.begin() + 8 * s.tri_first, 8 * s.tri_count,
                            cutout_records_.begin() + 8 * kept_cut);
            }
            kept_cut += s.tri_count;
        } else {
            if (kept != s.tri_first)
                std::copy_n(triangles_->begin() + 9 * s.tri_first, 9 * s.tri_count, triangles_->begin() + 9 * kept);
            kept += s.tri_count;
        }
        if (!z_draws_.empty() && z_draws_.back().batch == s.batch &&
            z_draws_.back().first + z_draws_.back().count == s.first)
            z_draws_.back().count += s.count;
        else
            z_draws_.push_back({s.first, s.count, s.batch});
    }
    triangles_->resize(9 * kept);
    cutout_xyz_.resize(9 * kept_cut);
    cutout_records_.resize(8 * kept_cut);
    // The shadow casters behind the camera, through the main camera only:
    // in the acceleration structure, not in the view z pass.
    if (casters_ && main_camera_ != ~0u) {
        const auto &main_key = cameras_[main_camera_].first;
        for (const rt::GeoPoly &poly : *casters_) {
            if (poly.num_vertices < 3) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            if (renderer == 1) continue;
            const std::array<float, 5> camera{float(crtc_x + poly.center[0]), float((384 - poly.center[1]) + crtc_y),
                                              poly.object_focus_x, poly.object_focus_y, float(poly.window)};
            if (camera != main_key || rt::Geo::hud_overlay_address(poly.object_oba)) continue;
            const bool cutout = renderer == 3 && !poly.local_texture;
            std::vector<float> &xyz = cutout ? cutout_xyz_ : *triangles_;
            if (cutout) {
                const uint16_t color = le16(mem.palram, m2gpu::polygon_palette_index(poly.texheader[3]));
                const auto material = m2gpu::pack_polygon_material(
                    poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.luma, poly.texlod, lod_bias, color);
                for (int k = 1; k + 1 < poly.num_vertices; k++) {
                    cutout_records_.insert(cutout_records_.end(), {material.info[0], material.info[1]});
                    for (int corner : {0, k, k + 1})
                        for (float t : {poly.v[corner].p[1] * 32.0f, poly.v[corner].p[2] * 32.0f}) {
                            uint32_t bits;
                            std::memcpy(&bits, &t, 4);
                            cutout_records_.push_back(bits);
                        }
                }
            }
            for (int k = 1; k + 1 < poly.num_vertices; k++)
                for (int corner : {0, k, k + 1}) {
                    const rt::GeoVertex &g = poly.v[corner];
                    xyz.insert(xyz.end(), {g.x, g.y, g.p[0]});
                }
        }
    }
}

bool GpuRenderer::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h,
                         const rt::Video &video, int scale, int pixel_w, int pixel_h,
                         bool sharp_textures, const std::vector<rt::GeoPoly> *pose_polys) {
    if (!ok()) return false;
    scale = std::clamp(scale, 1, 4);
    if (pixel_w <= 0 || pixel_h <= 0) {
        pixel_w = w * scale;
        pixel_h = h * scale;
    }
    // The 2x buffer resolves to its first mip. Select texture LOD for the
    // output pixels, and use the finer buffer only for coverage.
    const int output_h = sharp_textures ? pixel_h / 2 : pixel_h;
    const int lod_scale = std::clamp(int(std::lround(double(output_h) / h)), 1, 4);
    const auto &original = pose_polys ? *pose_polys : video.gpu_polys();
    const auto *replacement = have_opponents_ ? opponent_bodies_.replace(original) : nullptr;
    if (have_player_) {
        player_body_.set_shadow_light(car_shadows_ ? game_light_.data() : nullptr);
        if (const auto *player = player_body_.replace(replacement ? *replacement : original)) replacement = player;
    }
    // before build(): its materials ask for the pack only once it is on the GPU
    if (!hires_.entries.empty()) hires_upload(cmd, video.gpu_mem());
    build(video, w, h, lod_scale, sharp_textures, replacement ? *replacement : original);
    static std::FILE *trace = [] {
        const char *file = std::getenv("M2_OPPONENT_PRESENT_TRACE");
        if (!file || !*file) return static_cast<std::FILE *>(nullptr);
        auto *output = std::fopen(file, "w");
        if (output) std::fputs("time_ns,original_faces,presented_faces,vertices,replaced\n", output);
        return output;
    }();
    if (trace) {
        std::fprintf(trace, "%llu,%zu,%zu,%zu,%d\n",
                     static_cast<unsigned long long>(SDL_GetTicksNS()), original.size(),
                     replacement ? replacement->size() : original.size(), verts_.size(), replacement != nullptr);
    }
    const uint32_t vert_bytes = uint32_t(verts_.size() * sizeof(PolyVertex));
    if (!ensure(w, h, pixel_w, pixel_h, vert_bytes)) return false;

    // The tilemap pixmaps' rows to upload, per layer: the span of the tile
    // rows changed since tilepix_ was filled (everything for a new Video).
    const uint64_t tile_generation = video.system24_texture_generation();
    const bool all_tiles = video.instance() != tile_instance_ || tile_generation < tile_generation_;
    for (int l = 0; l < 4; l++) {
        int first = 64, last = -1;
        for (unsigned r = 0; r < 64; r++) {
            bool changed = all_tiles;
            for (unsigned t = r * 64; !changed && t < r * 64 + 64; t++)
                changed = video.system24_tile_generation(l, t) > tile_generation_;
            if (changed) first = std::min(first, int(r)), last = int(r);
        }
        tile_rows_[l] = last < 0 ? Rows{0, 0} : Rows{uint32_t(first) * 8, uint32_t(last - first + 1) * 8};
    }
    tile_instance_ = video.instance();
    tile_generation_ = tile_generation;

    // upload: the CPU front layers (HUD at the edges only), the layer quad,
    // the polygon vertices, the storage data
    const bool cpu_front = video.cpu_front();
    const uint32_t layer_bytes = uint32_t(w) * uint32_t(h) * 4;
    auto *p = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev_, upload_, true));
    uint32_t at = 0;
    if (cpu_front) {
        const std::vector<uint32_t> &front = video.foreground_layer();
        std::memcpy(p, front.data(), std::min(front.size() * 4, size_t(layer_bytes)));
        at = layer_bytes;
    }
    const float fw = float(w), fh = float(h);
    const QuadVertex quad[6] = {{0, 0, 0, 0}, {fw, 0, 1, 0}, {0, fh, 0, 1}, {fw, 0, 1, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1}};
    const uint32_t quad_at = at;
    std::memcpy(p + quad_at, quad, sizeof quad);
    const uint32_t verts_at = quad_at + uint32_t(sizeof quad);
    if (vert_bytes) std::memcpy(p + verts_at, verts_.data(), vert_bytes);
    at = verts_at + vert_bytes;
    // luma RAM and the gamma'd colour translation every frame, texture RAM when it changed
    const rt::VideoMem &mem = video.gpu_mem();
    const uint32_t data_at = at;
    const bool have_mem = mem.lumaram && mem.colorxlat && mem.tex0 && mem.tex1;
    bool tex_changed = false;
    if (have_mem) {
        std::memcpy(p + data_at, mem.lumaram, kLumaBytes);
        xlat_table_.resize(kXlatEntries);
        for (uint32_t i = 0; i < kXlatEntries; i++) xlat_table_[i] = gamma_[le16(mem.colorxlat, i) & 0xff];
        std::memcpy(p + data_at + kLumaBytes, xlat_table_.data(), kXlatEntries * 4);
        at += kLumaBytes + kXlatEntries * 4;
        tex_changed = mem.tex_generation != texram_generation_; // the board counts texture RAM writes
        if (tex_changed) {
            texram_generation_ = mem.tex_generation;
            std::memcpy(p + at, mem.tex0, kSheetWords * 4);
            std::memcpy(p + at + kSheetWords * 4, mem.tex1, kSheetWords * 4);
            at += kTexramBytes;
        }
    }
    // tilemaps: the changed pixmap rows, packed as m2.hlsl reads them (pen, category in bit 15)
    uint32_t rows_at[4];
    for (int l = 0; l < 4; l++) {
        rows_at[l] = at;
        const uint32_t n = tile_rows_[l].count * 512;
        if (!n) continue;
        const uint16_t *pixels = video.system24_pixels(l) + size_t(tile_rows_[l].first) * 512;
        const uint8_t *flags = video.system24_flags(l) + size_t(tile_rows_[l].first) * 512;
        uint16_t *dst = reinterpret_cast<uint16_t *>(p + at);
        for (uint32_t i = 0; i < n; i++) dst[i] = uint16_t(pixels[i] | (flags[i] & 1) << 15);
        at += n * 2;
    }
    // the sky panorama, when the matched one changed (or for a new Video)
    const uint32_t sky_at = at;
    const uint16_t *sky = video.sky_panorama();
    const bool sky_upload = sky && (all_tiles || video.sky_generation() != sky_generation_);
    if (sky_upload) {
        std::memcpy(p + sky_at, sky, kSkyBytes);
        at += kSkyBytes;
        sky_generation_ = video.sky_generation();
    }
    // the header (widescreen margin and its fill), pens, tile words
    tiledata_words_.assign(kTileDataWords, 0);
    tiledata_words_[0] = uint32_t(video.margin());
    tiledata_words_[1] = uint32_t(video.backdrop()); // Edges 0, Sky 1, Stretch 2, as m2.hlsl
    tiledata_words_[2] = uint32_t(scale);
    tiledata_words_[3] = uint32_t(pixel_w);
    tiledata_words_[4] = uint32_t(pixel_h);
    tiledata_words_[5] = uint32_t(video.sky_origin());
    tiledata_words_[6] = smooth_2d_ ? 1u : 0u; // m2.hlsl smooth_vote
    std::memcpy(&tiledata_words_[kTileHeader], video.gpu_pens(), rt::Video::kGpuPens * 4);
    std::memcpy(&tiledata_words_[kTileHeader + rt::Video::kGpuPens], video.gpu_tile_words(), rt::Video::kGpuTileWords * 2);
    const uint32_t tiledata_at = at;
    std::memcpy(p + tiledata_at, tiledata_words_.data(), kTileDataWords * 4);
    const bool upload_opponents = opponent_atlas_ && !opponent_atlas_uploaded_;
    const uint32_t opponent_at = tiledata_at + kTileDataWords * 4;
    if (upload_opponents)
        std::memcpy(p + opponent_at, atlas_cpu_.data(), atlas_cpu_.size());
    SDL_UnmapGPUTransferBuffer(dev_, upload_);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    // cycle: the whole buffer is replaced, so a buffer still in use may be swapped for a fresh one
    auto upload_buffer = [&](uint32_t from, SDL_GPUBuffer *buffer, uint32_t offset, uint32_t size, bool cycle = true) {
        SDL_GPUTransferBufferLocation src{upload_, from};
        SDL_GPUBufferRegion dst{buffer, offset, size};
        SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
    };
    if (cpu_front) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_;
        src.pixels_per_row = Uint32(w);
        SDL_GPUTextureRegion dst{};
        dst.texture = front_;
        dst.w = Uint32(w);
        dst.h = Uint32(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
    }
    upload_buffer(quad_at, qbuf_, 0, sizeof quad);
    if (vert_bytes) upload_buffer(verts_at, vbuf_, 0, vert_bytes);
    if (have_mem) {
        upload_buffer(data_at, luma_, 0, kLumaBytes);
        upload_buffer(data_at + kLumaBytes, xlat_, 0, kXlatEntries * 4);
        if (tex_changed) upload_buffer(data_at + kLumaBytes + kXlatEntries * 4, texram_, 0, kTexramBytes);
    }
    for (int l = 0; l < 4; l++)
        if (tile_rows_[l].count)
            upload_buffer(rows_at[l], tilepix_, (uint32_t(l) * 512 * 512 + tile_rows_[l].first * 512) * 2,
                          tile_rows_[l].count * 512 * 2, false); // rows: the rest must stay
    if (sky_upload) upload_buffer(sky_at, tilepix_, kTilePixBytes, kSkyBytes, false);
    upload_buffer(tiledata_at, tiledata_, 0, kTileDataWords * 4);
    if (upload_opponents) {
        upload_buffer(opponent_at, opponent_atlas_, 0, uint32_t(atlas_cpu_.size()));
        opponent_atlas_uploaded_ = true;
    }
    SDL_EndGPUCopyPass(copy);

    // the course's sky for the cars' paint: the back layers, then their mip levels
    std::fill(car_light_ + 24, car_light_ + 28, 0.0f);
    if (car_reflect_ && sky_env_pipe_ && sky_camera_[2] > 0.0f) {
        if (!sky_env_) {
            SDL_GPUTextureCreateInfo ki{};
            ki.type = SDL_GPU_TEXTURETYPE_2D;
            ki.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            ki.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ki.width = kSkyEnvW, ki.height = kSkyEnvH;
            ki.layer_count_or_depth = 1;
            ki.num_levels = kSkyEnvLevels;
            sky_env_ = SDL_CreateGPUTexture(dev_, &ki);
        }
        SDL_GPUColorTargetInfo kt{};
        kt.texture = sky_env_;
        kt.load_op = SDL_GPU_LOADOP_DONT_CARE;
        kt.store_op = SDL_GPU_STOREOP_STORE;
        if (SDL_GPURenderPass *kp = sky_env_ ? SDL_BeginGPURenderPass(cmd, &kt, 1, nullptr) : nullptr) {
            SDL_BindGPUGraphicsPipeline(kp, sky_env_pipe_);
            SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
            SDL_BindGPUFragmentStorageBuffers(kp, 0, storage, 2);
            SDL_DrawGPUPrimitives(kp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(kp);
            SDL_GenerateMipmapsForGPUTexture(cmd, sky_env_);
            std::copy(sky_camera_, sky_camera_ + 4, car_light_ + 24);
        }
    }

    // the cars' shadow map, drawn before the frame that reads it
    if (shadow_near_ + shadow_far_ && shadow_pipe_) {
        if (!shadow_map_) {
            SDL_GPUTextureCreateInfo st{};
            st.type = SDL_GPU_TEXTURETYPE_2D;
            st.format = shadow_format_;
            st.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            st.width = kShadowMapW, st.height = kShadowMapH;
            st.layer_count_or_depth = st.num_levels = 1;
            shadow_map_ = SDL_CreateGPUTexture(dev_, &st);
        }
        if (shadow_map_) {
            SDL_GPUDepthStencilTargetInfo sd{};
            sd.texture = shadow_map_;
            sd.clear_depth = 1.0f;
            sd.load_op = SDL_GPU_LOADOP_CLEAR;
            sd.store_op = SDL_GPU_STOREOP_STORE;
            sd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            sd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            if (SDL_GPURenderPass *sp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &sd)) {
                SDL_BindGPUGraphicsPipeline(sp, shadow_pipe_);
                SDL_GPUBufferBinding svb{vbuf_, 0};
                SDL_BindGPUVertexBuffers(sp, 0, &svb, 1);
                constexpr float half = float(kShadowMapW) / 2.0f;
                if (shadow_near_) { // the left half
                    const SDL_GPUViewport sv{0, 0, half, float(kShadowMapH), 0, 1};
                    SDL_SetGPUViewport(sp, &sv);
                    SDL_DrawGPUPrimitives(sp, shadow_near_, 1, shadow_first_, 0);
                }
                if (shadow_far_) { // the right
                    const SDL_GPUViewport sv{half, 0, half, float(kShadowMapH), 0, 1};
                    SDL_SetGPUViewport(sp, &sv);
                    SDL_DrawGPUPrimitives(sp, shadow_far_, 1, shadow_first_ + shadow_near_, 0);
                }
                SDL_EndGPURenderPass(sp);
            }
        } else {
            car_light_[22] = 0.0f;
        }
    }

    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = SDL_FColor{0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    // MSAA (not with the shadow passes, which read single-sample depth): drawn
    // into msaa_color_, resolved into resolve_, copied into the target (which
    // may be larger than the frame)
    const bool multisampled = msaa_ > 1 && !shadows_ && msaa_color_;
    if (multisampled) {
        ct.texture = msaa_color_;
        ct.store_op = SDL_GPU_STOREOP_RESOLVE;
        ct.resolve_texture = resolve_;
    }
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    shadow_ready_ = false;
    const bool shadow_inputs = shadows_ && !verts_.empty() && ensure_shadow_targets(pixel_w, pixel_h);
    dt.store_op = shadow_inputs ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE; // the z pass reads it
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (!pass) return false;
    // the target may be bigger than this frame; positions are in original
    // pixels (screen), the viewport and clip rectangles in the target's
    const SDL_GPUViewport viewport{0, 0, float(pixel_w), float(pixel_h), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const float screen[4] = {fw, fh, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    // the cars' reflections and shadows (m2.hlsl CarLight): towards the game's sun, camera space
    car_light_[0] = -game_light_[0], car_light_[1] = -game_light_[1], car_light_[2] = -game_light_[2];
    if (const float len = std::sqrt(car_light_[0] * car_light_[0] + car_light_[1] * car_light_[1] + car_light_[2] * car_light_[2]);
        len > 1e-6f)
        for (int a = 0; a < 3; ++a) car_light_[a] /= len;
    SDL_PushGPUFragmentUniformData(cmd, 0, car_light_, sizeof car_light_);
    const SDL_Rect full{0, 0, pixel_w, pixel_h};

    // a full-frame quad through `pipe`: the tilemap layers, or the CPU front layers
    auto draw_quad = [&](SDL_GPUGraphicsPipeline *pipe) {
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_SetGPUScissor(pass, &full);
        SDL_GPUBufferBinding qb{qbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
        if (pipe == quad_pipe_ || pipe == quad_smooth_pipe_) {
            SDL_GPUTextureSamplerBinding tsb{front_, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        } else {
            SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
        }
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    };
    draw_quad(back_pipe_);
    if (!verts_.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, poly_pipe_);
        SDL_GPUBufferBinding vb{vbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBuffer *storage[6] = {texram_, luma_, xlat_, opponent_atlas_ ? opponent_atlas_ : texram_,
                                     hires_index_ ? hires_index_ : texram_, hires_data_ ? hires_data_ : texram_};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 6);
        const SDL_GPUTextureSamplerBinding textures[2] = {
            {shadow_near_ + shadow_far_ && shadow_map_ ? shadow_map_ : shadow_none_, shadow_sampler_},
            {car_light_[26] > 0.0f ? sky_env_ : sky_none_, sky_sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
        for (const Batch &b : batches_) {
            if (!b.count) continue;
            const int x0 = b.clip.x * pixel_w / w;
            const int y0 = b.clip.y * pixel_h / h;
            const int x1 = (b.clip.x + b.clip.w) * pixel_w / w;
            const int y1 = (b.clip.y + b.clip.h) * pixel_h / h;
            if (x1 <= x0 || y1 <= y0) continue;
            const SDL_Rect clip{x0, y0, x1 - x0, y1 - y0};
            SDL_SetGPUScissor(pass, &clip);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
    draw_quad(cpu_front ? (smooth_2d_ ? quad_smooth_pipe_ : quad_pipe_) : front_pipe_);
    SDL_EndGPURenderPass(pass);
    if (multisampled) {
        SDL_GPUCopyPass *copy_out = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation from{};
        from.texture = resolve_;
        SDL_GPUTextureLocation to{};
        to.texture = target;
        SDL_CopyGPUTextureToTexture(copy_out, &from, &to, Uint32(pixel_w), Uint32(pixel_h), 1, false);
        SDL_EndGPUCopyPass(copy_out);
    }
    if (!shadow_inputs) return true;

    // Shadow inputs (dxr.hlsl step 1): the view z of the polygon that won
    // each pixel, by the depth test EQUAL against the pass above; 0 where a
    // front layer covers the 3D, or where nothing was drawn.
    SDL_GPUColorTargetInfo zt{};
    zt.texture = view_z_;
    zt.load_op = SDL_GPU_LOADOP_CLEAR;
    zt.clear_color = SDL_FColor{0, 0, 0, 0};
    zt.store_op = SDL_GPU_STOREOP_STORE;
    dt.load_op = SDL_GPU_LOADOP_LOAD;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass = SDL_BeginGPURenderPass(cmd, &zt, 1, &dt);
    if (!pass) return true; // the frame itself is drawn
    SDL_SetGPUViewport(pass, &viewport);
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    SDL_BindGPUGraphicsPipeline(pass, z_pipe_);
    SDL_GPUBufferBinding vb{vbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    for (const ZDraw &d : z_draws_) { // the main camera's polygons only
        const Batch &b = batches_[d.batch];
        const int x0 = b.clip.x * pixel_w / w;
        const int y0 = b.clip.y * pixel_h / h;
        const int x1 = (b.clip.x + b.clip.w) * pixel_w / w;
        const int y1 = (b.clip.y + b.clip.h) * pixel_h / h;
        if (x1 <= x0 || y1 <= y0) continue;
        const SDL_Rect clip{x0, y0, x1 - x0, y1 - y0};
        SDL_SetGPUScissor(pass, &clip);
        SDL_DrawGPUPrimitives(pass, d.count, 1, d.first, 0);
    }
    if (cpu_front) {
        SDL_BindGPUGraphicsPipeline(pass, z_quad_pipe_);
        SDL_GPUTextureSamplerBinding tsb{front_, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    } else {
        SDL_BindGPUGraphicsPipeline(pass, z_front_pipe_);
        SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
    }
    SDL_SetGPUScissor(pass, &full);
    SDL_GPUBufferBinding qb{qbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    shadow_frame_.width = pixel_w;
    shadow_frame_.height = pixel_h;
    shadow_frame_.to_screen[0] = float(w) / float(pixel_w);
    shadow_frame_.to_screen[1] = float(h) / float(pixel_h);
    if (main_camera_ >= cameras_.size()) return true;
    const auto camera = cameras_.begin() + main_camera_;
    shadow_frame_.origin[0] = camera->first[0];
    shadow_frame_.origin[1] = camera->first[1];
    shadow_frame_.focus[0] = camera->first[2];
    shadow_frame_.focus[1] = camera->first[3];
    shadow_frame_.cutout_xyz = &cutout_xyz_;
    shadow_frame_.cutout_records = &cutout_records_;
    shadow_frame_.tex0 = have_mem ? mem.tex0 : nullptr;
    shadow_frame_.tex1 = have_mem ? mem.tex1 : nullptr;
    shadow_frame_.tex_generation = mem.tex_generation;
    frame_w_ = w, frame_h_ = h;
    shadow_ready_ = true;
    return true;
}

bool GpuRenderer::enable_shadows() {
    if (shadows_) return true;
    if (!ok() || !(SDL_GetGPUShaderFormats(dev_) & SDL_GPU_SHADERFORMAT_DXIL)) return false;
    if (msaa_ > 1) return false; // its passes read single-sample depth (init's msaa)
    auto dxil = [&](const unsigned char *code, size_t size, const char *entry, SDL_GPUShaderStage stage,
                    int uniforms, int samplers, int storage_buffers) {
        SDL_GPUShaderCreateInfo si{};
        si.stage = stage;
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.code = code;
        si.code_size = size;
        si.entrypoint = entry;
        si.num_uniform_buffers = Uint32(uniforms);
        si.num_samplers = Uint32(samplers);
        si.num_storage_buffers = Uint32(storage_buffers);
        return SDL_CreateGPUShader(dev_, &si);
    };
    SDL_GPUShader *vs_poly = dxil(k_vs_poly_dxil, sizeof k_vs_poly_dxil, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0, 0);
    SDL_GPUShader *vs_quad = dxil(k_vs_quad_dxil, sizeof k_vs_quad_dxil, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0, 0);
    SDL_GPUShader *ps_z = dxil(k_ps_view_z_dxil, sizeof k_ps_view_z_dxil, "ps_view_z", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 0);
    SDL_GPUShader *ps_front = dxil(k_ps_front_z_dxil, sizeof k_ps_front_z_dxil, "ps_front_z",
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_quad = dxil(k_ps_quad_z_dxil, sizeof k_ps_quad_z_dxil, "ps_quad_z", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    SDL_GPUShader *ps_shade = dxil(k_ps_shade_dxil, sizeof k_ps_shade_dxil, "ps_shade", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    const bool shaders = vs_poly && vs_quad && ps_z && ps_front && ps_quad && ps_shade;

    SDL_GPUColorTargetDescription z_color{};
    z_color.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUVertexBufferDescription pvb{0, sizeof(PolyVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute pattr[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, ooz))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, Uint32(offsetof(PolyVertex, info))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, Uint32(offsetof(PolyVertex, more))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_z;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 6;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = false;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_EQUAL; // only the polygon that won the pixel
    pi.target_info.color_target_descriptions = &z_color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    if (shaders) z_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);

    SDL_GPUVertexBufferDescription qvb{0, sizeof(QuadVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &z_color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the z of the polygons
    if (shaders) {
        qi.fragment_shader = ps_front;
        z_front_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
        qi.fragment_shader = ps_quad;
        z_quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    }
    // the frame times the light, plus the light shafts: src.rgb + dst * src.a
    // (dxr.hlsl ps_shade), its own pass (no depth); without shafts src.rgb
    // is 0 and this is dst * light as before
    SDL_GPUColorTargetDescription shade{};
    shade.format = frame_format_;
    shade.blend_state.enable_blend = true;
    shade.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    shade.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    shade.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    shade.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    shade.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    shade.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    qi.target_info.color_target_descriptions = &shade;
    qi.target_info.has_depth_stencil_target = false;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    if (shaders) {
        qi.fragment_shader = ps_shade;
        shade_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    }
    for (SDL_GPUShader *s : {vs_poly, vs_quad, ps_z, ps_front, ps_quad, ps_shade})
        if (s) SDL_ReleaseGPUShader(dev_, s);
    if (!z_pipe_ || !z_front_pipe_ || !z_quad_pipe_ || !shade_pipe_) {
        error_ = std::string("shadow pipelines: ") + SDL_GetError();
        for (SDL_GPUGraphicsPipeline **p : {&z_pipe_, &z_front_pipe_, &z_quad_pipe_, &shade_pipe_})
            if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
        return false;
    }
    shadows_ = true;
    return true;
}

// The view z and light textures, the target's size; their D3D12 resources
// come back through the creation properties (patch 0002).
bool GpuRenderer::ensure_shadow_targets(int pixel_w, int pixel_h) {
    if (view_z_ && pixel_w == shadow_frame_.width && pixel_h == shadow_frame_.height && shadow_frame_.view_z &&
        shadow_frame_.light)
        return true;
    if (view_z_) SDL_ReleaseGPUTexture(dev_, view_z_), view_z_ = nullptr;
    if (light_) SDL_ReleaseGPUTexture(dev_, light_), light_ = nullptr;
    shadow_frame_ = ShadowFrame{};
    auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, void *&resource) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = format;
        ti.usage = usage;
        ti.width = Uint32(pixel_w);
        ti.height = Uint32(pixel_h);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.props = SDL_CreateProperties();
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(dev_, &ti);
        resource = SDL_GetPointerProperty(ti.props, "SDL.gpu.texture.d3d12.resource", nullptr);
        SDL_DestroyProperties(ti.props);
        return texture;
    };
    // both stay in SDL's default state for a sampled texture (all shader resource)
    view_z_ = make(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                   shadow_frame_.view_z);
    light_ = make(SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, // light, light shafts
                  SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE, shadow_frame_.light);
    shadow_frame_.width = pixel_w;
    shadow_frame_.height = pixel_h;
    return view_z_ && light_ && shadow_frame_.view_z && shadow_frame_.light;
}

// dxr.hlsl step 3: the target times the light the DXR pass wrote.
bool GpuRenderer::apply_shadow(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target) {
    if (!shadows_ || !light_ || !frame_w_) return false;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!pass) return false;
    const SDL_GPUViewport viewport{0, 0, float(shadow_frame_.width), float(shadow_frame_.height), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect full{0, 0, shadow_frame_.width, shadow_frame_.height};
    SDL_SetGPUScissor(pass, &full);
    const float screen[4] = {float(frame_w_), float(frame_h_), 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    SDL_BindGPUGraphicsPipeline(pass, shade_pipe_);
    SDL_GPUBufferBinding qb{qbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
    SDL_GPUTextureSamplerBinding tsb{light_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

// The display passes (present): made on first use, so that nothing of them
// exists while they are off.
namespace {
constexpr SDL_GPUTextureFormat kGlowFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

struct PostUniforms { // m2.hlsl Post
    float src_uv[2], src_lod, lines, step[2], dst_size[2], bloom, crt, threshold, pad;
};

SDL_GPUGraphicsPipeline *post_pipeline(SDL_GPUDevice *dev, SDL_GPUShader *vs, SDL_GPUShader *ps,
                                       SDL_GPUTextureFormat format) {
    SDL_GPUColorTargetDescription color{};
    color.format = format;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = ps;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.color_target_descriptions = &color;
    pi.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(dev, &pi);
}
} // namespace

bool GpuRenderer::init_post() {
    if (post_ps_) return true;
    if (post_failed_ || !dev_) return false;
    post_failed_ = true; // until everything is made
    post_vs_ = shader(k_vs_post_spv, sizeof k_vs_post_spv, k_vs_post_dxil, sizeof k_vs_post_dxil, k_vs_post_msl,
                      "vs_post", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader *bright = shader(k_ps_post_bright_spv, sizeof k_ps_post_bright_spv, k_ps_post_bright_dxil,
                                   sizeof k_ps_post_bright_dxil, k_ps_post_bright_msl, "ps_post_bright",
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *blur = shader(k_ps_post_blur_spv, sizeof k_ps_post_blur_spv, k_ps_post_blur_dxil,
                                 sizeof k_ps_post_blur_dxil, k_ps_post_blur_msl, "ps_post_blur",
                                 SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader *ps = shader(k_ps_post_spv, sizeof k_ps_post_spv, k_ps_post_dxil, sizeof k_ps_post_dxil,
                               k_ps_post_msl, "ps_post", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2);
    if (post_vs_ && bright && blur) {
        post_bright_pipe_ = post_pipeline(dev_, post_vs_, bright, kGlowFormat);
        post_blur_pipe_ = post_pipeline(dev_, post_vs_, blur, kGlowFormat);
    }
    for (SDL_GPUShader *sh : {bright, blur})
        if (sh) SDL_ReleaseGPUShader(dev_, sh);
    SDL_GPUSamplerCreateInfo smp{};
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_LINEAR;
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    smp.max_lod = 16;
    post_sampler_ = SDL_CreateGPUSampler(dev_, &smp);
    if (!post_vs_ || !ps || !post_bright_pipe_ || !post_blur_pipe_ || !post_sampler_) {
        std::fprintf(stderr, "daytona: the display passes unavailable: %s\n", SDL_GetError());
        if (ps) SDL_ReleaseGPUShader(dev_, ps);
        return false;
    }
    post_ps_ = ps;
    post_failed_ = false;
    return true;
}

bool GpuRenderer::present(SDL_GPUCommandBuffer *cmd, const PostFrame &f) {
    if (!f.source || !f.target || f.dest.w <= 0 || f.dest.h <= 0 || !init_post()) return false;
    SDL_GPUGraphicsPipeline *pipe = nullptr;
    for (auto &fp : post_pipes_)
        if (fp.first == f.format) pipe = fp.second;
    if (!pipe) {
        pipe = post_pipeline(dev_, post_vs_, post_ps_, f.format);
        if (!pipe) return false;
        post_pipes_.emplace_back(f.format, pipe);
    }
    PostUniforms u{};
    u.src_uv[0] = f.uv_w, u.src_uv[1] = f.uv_h;
    u.src_lod = f.lod;
    u.lines = float(f.lines);
    u.dst_size[0] = float(f.dest.w), u.dst_size[1] = float(f.dest.h);
    u.bloom = f.bloom, u.crt = f.crt, u.threshold = f.threshold;

    auto fullscreen = [&](SDL_GPUTexture *out, SDL_GPUGraphicsPipeline *p, SDL_GPUTexture *in, SDL_GPUTexture *glow,
                          const SDL_GPUViewport *viewport, bool clear) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = out;
        ct.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;
        ct.clear_color = SDL_FColor{0, 0, 0, 1};
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        if (!pass) return false;
        SDL_BindGPUGraphicsPipeline(pass, p);
        if (viewport) SDL_SetGPUViewport(pass, viewport);
        const SDL_GPUTextureSamplerBinding tex[2] = {{in, post_sampler_}, {glow ? glow : in, post_sampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, tex, p == pipe ? 2 : 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        return true;
    };

    if (f.bloom > 0) { // the glow, at a quarter of the viewport
        const int gw = std::max(1, f.dest.w / 4), gh = std::max(1, f.dest.h / 4);
        if (gw != glow_w_ || gh != glow_h_) {
            for (SDL_GPUTexture *&t : glow_)
                if (t) SDL_ReleaseGPUTexture(dev_, t), t = nullptr;
            SDL_GPUTextureCreateInfo gi{};
            gi.type = SDL_GPU_TEXTURETYPE_2D;
            gi.format = kGlowFormat;
            gi.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            gi.width = Uint32(gw), gi.height = Uint32(gh);
            gi.layer_count_or_depth = gi.num_levels = 1;
            glow_[0] = SDL_CreateGPUTexture(dev_, &gi);
            glow_[1] = SDL_CreateGPUTexture(dev_, &gi);
            glow_w_ = glow_[0] && glow_[1] ? gw : 0, glow_h_ = gh;
            if (!glow_w_) return false;
        }
        u.step[0] = 1.0f / float(gw), u.step[1] = 1.0f / float(gh); // a glow pixel, in the source's uv
        if (!fullscreen(glow_[0], post_bright_pipe_, f.source, nullptr, nullptr, false)) return false;
        u.src_uv[0] = u.src_uv[1] = 1, u.src_lod = 0; // the blur reads the glow textures whole
        for (int pass = 0; pass < 2; pass++) { // twice each way: a wider glow
            u.step[0] = 1.0f / float(gw), u.step[1] = 0;
            if (!fullscreen(glow_[1], post_blur_pipe_, glow_[0], nullptr, nullptr, false)) return false;
            u.step[0] = 0, u.step[1] = 1.0f / float(gh);
            if (!fullscreen(glow_[0], post_blur_pipe_, glow_[1], nullptr, nullptr, false)) return false;
        }
        u.src_uv[0] = f.uv_w, u.src_uv[1] = f.uv_h, u.src_lod = f.lod;
    }
    const SDL_GPUViewport vp{float(f.dest.x), float(f.dest.y), float(f.dest.w), float(f.dest.h), 0, 1};
    return fullscreen(f.target, pipe, f.source, f.bloom > 0 ? glow_[0] : nullptr, &vp, true);
}

bool GpuRenderer::draw_benchmark(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int pixel_w, int pixel_h,
                                 int layers) {
    if (!ok() || pixel_w <= 0 || pixel_h <= 0 || layers <= 0) return false;
    const float fw = float(pixel_w), fh = float(pixel_h);
    // the planes: from the bottom of the frame (near, z 1.5) to the top
    // (far, z 80), the texture (256 x 256, wrapping) repeated along them
    verts_.clear();
    auto material = m2gpu::pack_polygon_material(2u << 13 | 3u << 0 | 3u << 3 | 1u << 6 | 1u << 7, 0x20, 0, 0x80, 0, 0, 0x4210);
    if (msaa_ > 1) material.info[0] |= 1u << 25;
    material.info[0] |= aniso_bits_ << 26;
    for (int l = 0; l < layers; ++l) {
        const float depth = 1.0f - float(l + 1) / float(layers + 2); // back to front: each one passes the test
        const float shift = float(l) * 0.37f;
        auto corner = [&](float x, float y) {
            const float t = 1.0f - y / fh;            // 0 at the bottom, 1 at the top
            const float z = 1.5f + t * t * 78.5f;    // camera units
            const float ooz = 1.0f / z;
            const float u = (x / fw - 0.5f) * 2048.0f, v = (z + shift) * 512.0f;
            PolyVertex c{};
            c.x = x, c.y = y, c.depth = depth;
            c.ooz = ooz, c.uoz = u * ooz / 8.0f, c.voz = v * ooz / 8.0f;
            c.r = c.g = c.b = c.a = 1.0f;
            for (int k = 0; k < 4; ++k) c.info[k] = material.info[size_t(k)];
            c.more[0] = material.more[0], c.more[1] = material.more[1];
            return c;
        };
        constexpr int kRows = 16; // strips, so that 1/z follows the curve
        for (int r = 0; r < kRows; ++r) {
            const float y0 = fh * float(r) / kRows, y1 = fh * float(r + 1) / kRows;
            const PolyVertex a = corner(0, y0), b = corner(fw, y0), c = corner(fw, y1), d = corner(0, y1);
            for (const PolyVertex &v : {a, b, c, a, c, d}) verts_.push_back(v);
        }
    }
    const uint32_t vert_bytes = uint32_t(verts_.size() * sizeof(PolyVertex));
    if (!ensure(1, 1, pixel_w, pixel_h, vert_bytes)) return false; // no layers: a 1 x 1 front texture

    auto *p = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev_, upload_, true));
    if (!p) return false;
    std::memcpy(p, verts_.data(), vert_bytes);
    const bool fill = !bench_filled_;
    if (fill) { // once: blocks of noise (texels that vary as a texture's do, not each one at random)
        uint32_t x = 0x9e3779b9u;
        auto *words = reinterpret_cast<uint32_t *>(p + vert_bytes);
        const uint32_t n = (kTexramBytes + kLumaBytes + kXlatEntries * 4) / 4;
        for (uint32_t i = 0; i < n; ++i) {
            if (!(i & 15u)) x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            const uint32_t v = x + i * 0x01010101u;
            words[i] = i >= (kTexramBytes + kLumaBytes) / 4 ? v & 0xffu : v;
        }
    }
    SDL_UnmapGPUTransferBuffer(dev_, upload_);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    auto upload_buffer = [&](uint32_t from, SDL_GPUBuffer *buffer, uint32_t size) {
        SDL_GPUTransferBufferLocation src{upload_, from};
        SDL_GPUBufferRegion dst{buffer, 0, size};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    };
    upload_buffer(0, vbuf_, vert_bytes);
    if (fill) {
        upload_buffer(vert_bytes, texram_, kTexramBytes);
        upload_buffer(vert_bytes + kTexramBytes, luma_, kLumaBytes);
        upload_buffer(vert_bytes + kTexramBytes + kLumaBytes, xlat_, kXlatEntries * 4);
        bench_filled_ = true;
        texram_generation_ = ~0ull;
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = SDL_FColor{0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    const bool multisampled = msaa_ > 1 && msaa_color_;
    if (multisampled) {
        ct.texture = msaa_color_;
        ct.store_op = SDL_GPU_STOREOP_RESOLVE;
        ct.resolve_texture = resolve_;
    }
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (!pass) return false;
    const SDL_GPUViewport viewport{0, 0, fw, fh, 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect full{0, 0, pixel_w, pixel_h};
    SDL_SetGPUScissor(pass, &full);
    const float screen[4] = {fw, fh, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    float light[sizeof car_light_ / sizeof(float)] = {0, 1, 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, light, sizeof light);
    SDL_BindGPUGraphicsPipeline(pass, poly_pipe_);
    SDL_GPUBufferBinding vb{vbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_GPUBuffer *storage[6] = {texram_, luma_, xlat_, texram_, texram_, texram_};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 6);
    const SDL_GPUTextureSamplerBinding textures[2] = {{shadow_none_, shadow_sampler_}, {sky_none_, sky_sampler_}};
    SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
    SDL_DrawGPUPrimitives(pass, Uint32(verts_.size()), 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    if (multisampled) {
        SDL_GPUCopyPass *copy_out = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation from{};
        from.texture = resolve_;
        SDL_GPUTextureLocation to{};
        to.texture = target;
        SDL_CopyGPUTextureToTexture(copy_out, &from, &to, Uint32(pixel_w), Uint32(pixel_h), 1, false);
        SDL_EndGPUCopyPass(copy_out);
    }
    return true;
}

void GpuRenderer::clear_hires_textures() { load_hires_textures(""); }

bool GpuRenderer::load_hires_textures(const std::string &path) {
    for (SDL_GPUBuffer **b : {&hires_index_, &hires_data_})
        if (*b) SDL_ReleaseGPUBuffer(dev_, *b), *b = nullptr;
    hires_ = hires::Pack{};
    hires_data_uploaded_ = false;
    hires_generation_ = ~0ull;
    hires_index_ready_ = false;
    hires_indexer_.reset();
    hires_live_ = 0;
    if (path.empty()) return true;
    if (!hires_.load(path)) {
        error_ = hires_.error;
        hires_ = hires::Pack{};
        return false;
    }
    if (hires_.scale != 4) { // m2.hlsl kHiresScale
        error_ = "high-resolution texture pack: scale " + std::to_string(hires_.scale) + ", 4 expected";
        hires_ = hires::Pack{};
        return false;
    }
    std::printf("daytona: high-resolution textures: %zu in the pack (%.1f MB)\n", hires_.entries.size(),
                double(hires_.data.size()) / 1e6);
    return true;
}

// The pack's tones, once; its index whenever texture RAM changed (the
// entries whose texels are there now). Own transfer buffers: rare uploads.
void GpuRenderer::hires_upload(SDL_GPUCommandBuffer *cmd, const rt::VideoMem &mem) {
    static uint64_t calls = 0; // the frames drawn with a pack, for M2_HIRES_TRACE
    ++calls;
    if (!mem.tex0 || !mem.tex1) return;
    auto upload = [&](SDL_GPUBuffer *&buffer, const void *bytes, uint32_t size) {
        if (!buffer) {
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            bi.size = size;
            buffer = SDL_CreateGPUBuffer(dev_, &bi);
            if (!buffer) return false;
        }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = size;
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev_, &ti);
        if (!tb) return false;
        void *p = SDL_MapGPUTransferBuffer(dev_, tb, false);
        if (p) {
            std::memcpy(p, bytes, size);
            SDL_UnmapGPUTransferBuffer(dev_, tb);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{tb, 0};
            SDL_GPUBufferRegion dst{buffer, 0, size};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
            SDL_EndGPUCopyPass(copy);
        }
        SDL_ReleaseGPUTransferBuffer(dev_, tb); // freed once the upload is done
        return p != nullptr; // not mapped: nothing was uploaded
    };
    if (!hires_data_uploaded_) {
        std::vector<uint8_t> padded(hires_.data);
        padded.resize((padded.size() + 3) & ~size_t(3));
        hires_data_uploaded_ = upload(hires_data_, padded.data(), uint32_t(padded.size()));
        if (!hires_data_uploaded_) {
            std::fprintf(stderr, "daytona: high-resolution textures unavailable: %s\n", SDL_GetError());
            hires_ = hires::Pack{};
            return;
        }
    }
    if (mem.tex_generation == hires_generation_ && hires_index_ready_) return;
    // the index for this texture RAM: until it is on the GPU the materials
    // ask for no replacement (build), so a failed upload draws the game's
    // own textures and is tried again next frame, never a stale index
    hires_index_ready_ = false;
    const uint64_t t0 = SDL_GetTicksNS();
    const size_t live = hires_indexer_.refresh(hires_, mem.tex0, mem.tex1, hires_words_); // the changed cells only
    static int fail_uploads = [] { // a test hook: M2_TEST_HIRES_INDEX_FAIL=N fails the first N index uploads
        const char *v = std::getenv("M2_TEST_HIRES_INDEX_FAIL");
        return v ? std::atoi(v) : 0;
    }();
    if (fail_uploads > 0) {
        --fail_uploads;
        std::fprintf(stderr, "daytona: high-resolution textures: index upload failed (test), the game's textures drawn\n");
        return;
    }
    if (!upload(hires_index_, hires_words_.data(), uint32_t(hires_words_.size() * 4))) {
        std::fprintf(stderr, "daytona: high-resolution textures: index upload failed (%s), the game's textures drawn\n",
                     SDL_GetError());
        return;
    }
    hires_index_ready_ = true;
    hires_generation_ = mem.tex_generation;
    if (live != hires_live_)
        std::printf("daytona: high-resolution textures: %zu of %zu in use (checked in %.1f ms)\n", live,
                    hires_.entries.size(), double(SDL_GetTicksNS() - t0) / 1e6);
    static std::FILE *trace = [] { // diagnostic: M2_HIRES_TRACE=FILE, per index refresh: frame, generation, live, rehashed, ms
        const char *f = std::getenv("M2_HIRES_TRACE");
        return f && *f ? std::fopen(f, "w") : nullptr;
    }();
    if (trace) std::fprintf(trace, "%llu,%llu,%zu,%zu,%.3f\n", static_cast<unsigned long long>(calls),
                            static_cast<unsigned long long>(mem.tex_generation), live, hires_indexer_.last_rehashed(),
                            double(SDL_GetTicksNS() - t0) / 1e6);
    hires_live_ = live;
}

} // namespace app
