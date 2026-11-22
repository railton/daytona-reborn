#include "app/gpu/home_scene.h"

#include "app/gpu/home_shaders_gen.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <vector>

namespace app {
namespace {

constexpr float tau = 2 * std::numbers::pi_v<float>;
constexpr auto color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr auto depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

struct Vec {
    float x, y, z;
    Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec operator*(float s) const { return {x * s, y * s, z * s}; }
};

float dot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec cross(Vec a, Vec b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
Vec unit(Vec v) { return v * (1 / std::sqrt(dot(v, v))); }
struct Vec4 { float x, y, z, w; };
Vec4 pack(Vec v, float w) { return {v.x, v.y, v.z, w}; }

struct Vertex { Vec pos, normal; Vec4 color; };
using Mesh = std::vector<Vertex>;

Vec4 pigment(unsigned rgb, float paint = 0) {
    return {float((rgb >> 16) & 255) / 255, float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255, paint};
}

void tri(Mesh &m, Vec a, Vec b, Vec c, Vec4 color, Vec n) {
    m.insert(m.end(), {{a, n, color}, {b, n, color}, {c, n, color}});
}

void face(Mesh &m, Vec a, Vec b, Vec c, Vec d, Vec4 color) {
    const Vec n = unit(cross(b - a, c - a));
    tri(m, a, b, c, color, n);
    tri(m, a, c, d, color, n);
}

void block(Mesh &m, Vec a, Vec b, Vec4 c) {
    const Vec p{a.x, b.y, a.z}, q{a.x, b.y, b.z}, r{b.x, b.y, a.z};
    const Vec s{a.x, a.y, b.z}, t{b.x, a.y, b.z}, u{b.x, a.y, a.z};
    face(m, p, q, b, r, c);
    face(m, a, s, q, p, c);
    face(m, t, u, r, b, c);
    face(m, s, t, b, q, c);
    face(m, u, a, p, r, c);
}

Vec track(float a) {
    return {32 * std::cos(a) + 2.5f * std::cos(2*a), 0, 19 * std::sin(a) - 2.8f * std::sin(2*a)};
}

Vec tangent(float a) { return unit(track(a + 0.001f) - track(a - 0.001f)); }
Vec lane(float a, float offset, float height) {
    const Vec t = tangent(a);
    return track(a) + Vec{-t.z * offset, height, t.x * offset};
}

void strip(Mesh &m, float a, float b, float lo, float hi, float y, unsigned color) {
    face(m, lane(a, hi, y), lane(b, hi, y), lane(b, lo, y), lane(a, lo, y), pigment(color));
}

void tree(Mesh &m, float x, float z, float h, unsigned tint) {
    block(m, {x - 0.13f, 0, z - 0.13f}, {x + 0.13f, h * 0.6f, z + 0.13f}, pigment(0x82594a));
    for (int layer = 0; layer < 3; ++layer) {
        const float base = h * (0.26f + layer * 0.21f);
        const float radius = h * (0.28f - layer * 0.054f);
        for (int i = 0; i < 6; ++i) {
            const float a = i * tau / 6, b = (i + 1) * tau / 6;
            const Vec p{x + radius * std::cos(a), base, z + radius * std::sin(a)};
            const Vec q{x + radius * std::cos(b), base, z + radius * std::sin(b)};
            const Vec tip{x, base + h * 0.34f, z};
            tri(m, p, tip, q, pigment(tint), unit(cross(tip - p, q - p)));
        }
    }
}

void build_track(Mesh &m) {
    face(m, {-230,-0.06f,-200}, {-230,-0.06f,160}, {230,-0.06f,160}, {230,-0.06f,-200}, pigment(0x85be76));
    for (int i = -14; i < 14; ++i) {
        const float x = i * 6.f;
        face(m, {x,-0.04f,-100}, {x,-0.04f,95}, {x+2.6f,-0.04f,95}, {x+2.6f,-0.04f,-100}, pigment(0x8cc37a));
    }
    constexpr int pieces = 160;
    for (int i = 0; i < pieces; ++i) {
        const float a = i * tau / pieces, b = (i+1) * tau / pieces;
        strip(m, a, b, -6.0f, 6.0f, 0.015f, i % 3 ? 0x666b78 : 0x686e7a);
        strip(m, a, b, -6.55f, -6.0f, 0.065f, (i / 2) % 2 ? 0xf4f5f0 : 0xe9584e);
        strip(m, a, b, 6.0f, 6.55f, 0.065f, (i / 2) % 2 ? 0xf4f5f0 : 0xe9584e);
        if (i % 4 < 2) strip(m, a, b, -0.055f, 0.055f, 0.034f, 0xc9ccd3);
        strip(m, a, b, -5.67f, -5.56f, 0.032f, 0xe7e9ef);
        strip(m, a, b, 5.56f, 5.67f, 0.032f, 0xe7e9ef);
        const float rail = -7.03f;
        face(m, lane(a, rail, 0.28f), lane(b, rail, 0.28f),
             lane(b, rail, 0.89f), lane(a, rail, 0.89f), pigment(0xb8c4cf));
        strip(m, a, b, rail - 0.12f, rail + 0.12f, 0.89f, 0xe9edf6);
        if (i % 4 == 0) {
            const Vec p = lane(a, rail, 0);
            block(m, p + Vec{-0.12f,0,-0.12f}, p + Vec{0.12f,1.02f,0.12f}, pigment(0x9eabb4));
        }
    }
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 16; ++i) {
            const float a = 1.80f + row * 0.025f, s = -5.52f + i * 0.69f;
            strip(m, a, a + 0.025f, s, s + 0.69f, 0.041f, (row+i) % 2 ? 0x262c39 : 0xf4f5ed);
        }
    // Grandstand and pit building overlook the back straight.
    for (int i = 0; i < 7; ++i) {
        const float z = -8.8f + i * 0.73f;
        block(m, {-19,0,z}, {12,3.8f - i * 0.45f,z+0.73f}, pigment(i % 2 ? 0xe2eaf1 : 0x87aeda));
    }
    for (int i = 0; i < 7; ++i) {
        const float x = -18.5f + i*5;
        block(m, {x,0,-9.1f}, {x+0.16f,5.5f,-8.94f}, pigment(0xdee6ef));
    }
    block(m, {-20,5.2f,-10}, {13,5.54f,-5}, pigment(0x4571b4));
    block(m, {-20,5.08f,-5.04f}, {13,5.55f,-4.84f}, pigment(0xd95048));
    block(m, {15,0,-7.4f}, {22,2.7f,-2.7f}, pigment(0xefdfc4));
    block(m, {14.7f,2.7f,-7.7f}, {22.3f,2.96f,-2.4f}, pigment(0xeb6c57));
    for (int i = 0; i < 4; ++i)
        block(m, {15.6f+i*1.6f,1.1f,-2.72f}, {16.7f+i*1.6f,2.1f,-2.66f}, pigment(0x4679a1));

    const unsigned greens[] = {0x328c64, 0x4b9f66, 0x63ab67, 0x26745b};
    for (int i = 0; i < 180; ++i) {
        const float a = float((i * 173) % 181) * tau / 181;
        const float offset = -10.5f - float((i * 113) % 89) * 0.17f;
        const Vec p = lane(a, offset, 0);
        if (p.z > 13 && std::abs(p.x) < 26 && i % 5 != 0) continue;
        tree(m, p.x, p.z, 2.5f + float(i % 7)*0.43f, greens[i % 4]);
    }
    for (int i = 0; i < 15; ++i)
        tree(m, -21.f + i * 2.7f, -0.8f + std::sin(float(i)) * 2.4f,
             1.8f + float(i % 4)*0.46f, greens[i % 4]);
}

void build_car(Mesh &m) {
    block(m, {-1.10f,0.015f,-1.91f}, {1.10f,0.027f,1.93f}, pigment(0x454853));
    block(m, {-0.90f,0.23f,-1.80f}, {0.90f,0.64f,1.72f}, pigment(0xf9f9f4, 0.38f));
    block(m, {-0.89f,0.55f,-1.67f}, {0.89f,0.83f,1.33f}, pigment(0xffffff, 1));
    block(m, {-0.91f,0.25f,1.32f}, {0.91f,0.57f,1.82f}, pigment(0xffffff, 1));
    const Vec a{-0.77f,0.83f,-0.92f}, b{-0.77f,0.83f,0.88f}, c{0.77f,0.83f,0.88f}, d{0.77f,0.83f,-0.92f};
    const Vec A{-0.61f,1.27f,-0.52f}, B{-0.61f,1.27f,0.33f}, C{0.61f,1.27f,0.33f}, D{0.61f,1.27f,-0.52f};
    face(m, A,B,C,D,pigment(0xffffff, 1));
    face(m, b,c,C,B,pigment(0x2f516b));
    face(m, d,a,A,D,pigment(0x294b6a));
    face(m, a,b,B,A,pigment(0x21364c));
    face(m, c,d,D,C,pigment(0x21364c));
    block(m, {-0.91f,0.96f,-1.75f}, {0.91f,1.05f,-1.49f}, pigment(0x1c2a3b));
    block(m, {-0.15f,0.834f,0.89f}, {0.16f,0.847f,1.32f}, pigment(0xfff2c8));
    for (float x : {-0.85f, 0.85f}) {
        block(m, {x-0.15f,0.41f,1.817f}, {x+0.03f,0.51f,1.84f}, pigment(0xffffdb));
        for (float z : {-1.12f, 1.13f}) {
            for (int i = 0; i < 12; ++i) {
                const float a0 = i * tau / 12, a1 = (i + 1) * tau / 12;
                const Vec outer{x + std::copysign(0.17f,x),0.34f,z};
                const Vec p = outer + Vec{0, 0.32f * std::cos(a0), 0.32f * std::sin(a0)};
                const Vec q = outer + Vec{0, 0.32f * std::cos(a1), 0.32f * std::sin(a1)};
                const Vec width{-std::copysign(0.34f,x),0,0};
                const Vec n{0,std::cos((a0+a1)/2),std::sin((a0+a1)/2)};
                tri(m,p,q,q+width,pigment(0x242733),n);
                tri(m,p,q+width,p+width,pigment(0x242733),n);
                tri(m,p,q,outer,pigment(0x3e4251),{std::copysign(1.f,x),0,0});
                tri(m,(p-outer)*0.56f+outer+Vec{std::copysign(0.003f,x),0,0},
                    (q-outer)*0.56f+outer+Vec{std::copysign(0.003f,x),0,0},
                    outer+Vec{std::copysign(0.003f,x),0,0},pigment(0xaeb7c2),{std::copysign(1.f,x),0,0});
            }
        }
    }
}

template<size_t S, size_t D>
SDL_GPUShader *shader(SDL_GPUDevice *dev, SDL_GPUShaderStage stage, Uint32 uniforms, Uint32 samplers,
                      const unsigned char (&spv)[S], const unsigned char (&dxil)[D], const char *entry) {
    SDL_GPUShaderCreateInfo info{};
    info.stage = stage;
    info.num_uniform_buffers = uniforms;
    info.num_samplers = samplers;
    const auto formats = SDL_GetGPUShaderFormats(dev);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spv;
        info.code_size = S;
        info.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = dxil;
        info.code_size = D;
        info.entrypoint = entry;
    } else {
        return nullptr;
    }
    return SDL_CreateGPUShader(dev, &info);
}

SDL_GPUGraphicsPipeline *pipeline(SDL_GPUDevice *dev, SDL_GPUShader *vs, SDL_GPUShader *ps,
                                  SDL_GPUTextureFormat format, bool geometry) {
    if (!vs || !ps) return nullptr;
    SDL_GPUVertexBufferDescription stream{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attrs[] = {
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,pos)},
        {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(Vertex,normal)},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(Vertex,color)},
    };
    SDL_GPUColorTargetDescription color{};
    color.format = format;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs;
    info.fragment_shader = ps;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    if (geometry) {
        info.vertex_input_state = {&stream, 1, attrs, 3};
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.depth_stencil_state.enable_depth_test = info.depth_stencil_state.enable_depth_write = true;
        info.target_info.depth_stencil_format = depth_format;
        info.target_info.has_depth_stencil_target = true;
    }
    return SDL_CreateGPUGraphicsPipeline(dev, &info);
}

} // namespace

bool HomeScene::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat swap_format) {
    dev_ = dev;
    auto *vs = shader(dev, SDL_GPU_SHADERSTAGE_VERTEX, 2, 0, k_vs_home_spv, k_vs_home_dxil, "vs_home");
    auto *ps = shader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, k_ps_home_spv, k_ps_home_dxil, "ps_home");
    auto *lens_vs = shader(dev, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, k_vs_lens_spv, k_vs_lens_dxil, "vs_lens");
    auto *lens_ps = shader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, k_ps_lens_spv, k_ps_lens_dxil, "ps_lens");
    world_ = pipeline(dev, vs, ps, color_format, true);
    lens_ = pipeline(dev, lens_vs, lens_ps, swap_format, false);
    for (auto *s : {vs, ps, lens_vs, lens_ps}) if (s) SDL_ReleaseGPUShader(dev, s);
    if (!world_ || !lens_) return false;

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev, &sampler);
    Mesh mesh;
    build_track(mesh);
    track_vertices_ = Uint32(mesh.size());
    build_car(mesh);
    car_vertices_ = Uint32(mesh.size()) - track_vertices_;
    const Uint32 bytes = Uint32(mesh.size() * sizeof(Vertex));
    SDL_GPUBufferCreateInfo vertex_info{SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0};
    vertices_ = SDL_CreateGPUBuffer(dev, &vertex_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    auto *transfer = SDL_CreateGPUTransferBuffer(dev, &transfer_info);
    bool uploaded = false;
    if (sampler_ && vertices_ && transfer) {
        if (void *p = SDL_MapGPUTransferBuffer(dev, transfer, false)) {
            std::memcpy(p, mesh.data(), bytes);
            SDL_UnmapGPUTransferBuffer(dev, transfer);
            if (auto *cmd = SDL_AcquireGPUCommandBuffer(dev)) {
                auto *copy = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTransferBufferLocation from{transfer,0};
                SDL_GPUBufferRegion to{vertices_,0,bytes};
                SDL_UploadToGPUBuffer(copy, &from, &to, false);
                SDL_EndGPUCopyPass(copy);
                uploaded = SDL_SubmitGPUCommandBuffer(cmd);
            }
        }
    }
    if (transfer) SDL_ReleaseGPUTransferBuffer(dev, transfer);
    return uploaded;
}

bool HomeScene::resize(Uint32 w, Uint32 h) {
    if (w == w_ && h == h_) return color_ && depth_;
    w_ = w;
    h_ = h;
    if (color_) SDL_ReleaseGPUTexture(dev_, color_);
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, color_format,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
    color_ = SDL_CreateGPUTexture(dev_, &info);
    info.format = depth_format;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_ = SDL_CreateGPUTexture(dev_, &info);
    if (!color_ || !depth_) SDL_Log("Home scene targets: %s", SDL_GetError());
    return color_ && depth_;
}

bool HomeScene::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swap, Uint32 w, Uint32 h,
                       double seconds, float visibility) {
    if (!resize(w, h)) return false;
    const float t = float(seconds);
    const float drift = std::sin(t * tau / 32);
    const Vec eye{12.f + 13.f * drift, 29.f + 3.f * std::cos(t * tau / 40), 51.f};
    const Vec look{0.f,0.f,8.3f};
    const Vec forward = unit(look - eye), right = unit(cross(forward, {0,1,0})), up = cross(right,forward);
    const float aspect = float(w) / float(h);
    const float shift = std::clamp((aspect - 0.85f) * 0.33f, 0.0f, 0.41f);
    const Vec4 camera[] = {pack(eye,0),pack(right,2.04f/aspect),pack(up,2.04f),pack(forward,shift),
                           {-0.16f,0.67f,0.72f,12.2f}};
    SDL_GPUColorTargetInfo ct{};
    ct.texture = color_;
    ct.clear_color = {0.52f,0.75f,0.84f,1};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    auto *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_BindGPUGraphicsPipeline(pass, world_);
    SDL_GPUBufferBinding binding{vertices_,0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, camera, sizeof camera);
    const Vec4 track_pose[] = {{0,0,0,1},{0,0,0,0}};
    SDL_PushGPUVertexUniformData(cmd, 1, track_pose, sizeof track_pose);
    SDL_DrawGPUPrimitives(pass, track_vertices_, 1, 0, 0);
    constexpr unsigned colors[] = {0xef453b,0x2d7bdb,0xf1b331,0xf5eadd,0x25aa85,0xec7042,0x6d71d6,0xf4d24d};
    for (unsigned i = 0; i < std::size(colors); ++i) {
        const float lap = i % 2 ? 16.f : 15.f;
        const float phase = 0.96f + float(i) * 0.77f + float(std::fmod(seconds, lap)) * tau / lap;
        const Vec direction = tangent(phase);
        auto paint = pigment(colors[i]);
        paint.w = direction.x;
        const Vec4 pose[] = {pack(lane(phase, i % 2 ? -2.8f : 2.8f, 0.02f),direction.z),paint};
        SDL_PushGPUVertexUniformData(cmd, 1, pose, sizeof pose);
        SDL_DrawGPUPrimitives(pass, car_vertices_, 1, track_vertices_, 0);
    }
    SDL_EndGPURenderPass(pass);
    ct.texture = swap;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, lens_);
    const SDL_GPUTextureSamplerBinding sampler{color_,sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
    const Vec4 lens{aspect,visibility,0,0};
    SDL_PushGPUFragmentUniformData(cmd, 0, &lens, sizeof lens);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

void HomeScene::shutdown() {
    if (!dev_) return;
    if (world_) SDL_ReleaseGPUGraphicsPipeline(dev_, world_);
    if (lens_) SDL_ReleaseGPUGraphicsPipeline(dev_, lens_);
    if (color_) SDL_ReleaseGPUTexture(dev_, color_);
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_);
    if (vertices_) SDL_ReleaseGPUBuffer(dev_, vertices_);
    dev_ = nullptr;
}

} // namespace app
