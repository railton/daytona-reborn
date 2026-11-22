#pragma once

#include <SDL3/SDL_gpu.h>
#include "app/gpu/hdr10_shaders_gen.h"
#include "app/hdr10_encode.h"

namespace app {

class Hdr10Present {
public:
    static constexpr auto scene_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    static constexpr auto output_format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;

    // display_gamma: 0 keeps the sRGB curve (the reference); otherwise the
    // game's levels are shown through a power law (an arcade CRT: ~2.4).
    // sdr: an 8-bit SDR output re-encoded for the sRGB display, instead of
    // PQ, for the display curve without HDR10.
    // display_saturation: 1 leaves the colours as they are; above 1 more vivid.
    explicit Hdr10Present(SDL_GPUDevice *device, float paper_white = kHdr10PaperWhiteNits,
                          float display_gamma = 0.0f, bool sdr = false, float display_saturation = 1.0f)
        : device_(device), uniforms_{paper_white, display_gamma, display_saturation, 0.0f}, sdr_(sdr) {}
    float display_gamma() const { return uniforms_.display_gamma; }
    Hdr10Present(const Hdr10Present &) = delete;
    Hdr10Present &operator=(const Hdr10Present &) = delete;
    ~Hdr10Present() { reset(); }

    bool init() {
        SDL_GPUShaderCreateInfo shader{};
        shader.format = SDL_GPU_SHADERFORMAT_DXIL;
        shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        shader.entrypoint = "vs_hdr10";
        shader.code = k_vs_hdr10_dxil;
        shader.code_size = sizeof(k_vs_hdr10_dxil);
        auto *vs = SDL_CreateGPUShader(device_, &shader);
        if (!vs) return false;
        shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        shader.entrypoint = sdr_ ? "ps_sdr_curve" : "ps_hdr10";
        shader.code = sdr_ ? k_ps_sdr_curve_dxil : k_ps_hdr10_dxil;
        shader.code_size = sdr_ ? sizeof(k_ps_sdr_curve_dxil) : sizeof(k_ps_hdr10_dxil);
        shader.num_samplers = 1;
        shader.num_uniform_buffers = 1;
        auto *ps = SDL_CreateGPUShader(device_, &shader);
        if (ps) {
            SDL_GPUColorTargetDescription color{};
            color.format = sdr_ ? scene_format : output_format;
            SDL_GPUGraphicsPipelineCreateInfo pipeline{};
            pipeline.vertex_shader = vs;
            pipeline.fragment_shader = ps;
            pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipeline.target_info.color_target_descriptions = &color;
            pipeline.target_info.num_color_targets = 1;
            pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);
            SDL_ReleaseGPUShader(device_, ps);
        }
        SDL_ReleaseGPUShader(device_, vs);
        if (!pipeline_) return false;

        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = SDL_CreateGPUSampler(device_, &sampler);
        return sampler_ != nullptr;
    }

    SDL_GPUTexture *scene(Uint32 width, Uint32 height) {
        if (scene_ && width == width_ && height == height_) return scene_;
        const SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, scene_format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            width, height, 1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
        auto *next = SDL_CreateGPUTexture(device_, &info);
        if (!next) return nullptr;
        if (scene_) SDL_ReleaseGPUTexture(device_, scene_);
        scene_ = next;
        width_ = width;
        height_ = height;
        return scene_;
    }

    bool compose(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *output) const {
        SDL_GPUColorTargetInfo color{};
        color.texture = output;
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        auto *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
        if (!pass) return false;
        SDL_BindGPUGraphicsPipeline(pass, pipeline_);
        const SDL_GPUTextureSamplerBinding source{scene_, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms_, sizeof(uniforms_));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        return true;
    }

    void reset() {
        if (scene_) SDL_ReleaseGPUTexture(device_, scene_);
        if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
        if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        scene_ = nullptr;
        sampler_ = nullptr;
        pipeline_ = nullptr;
    }

private:
    struct Uniforms {
        float paper_white;
        float display_gamma;
        float display_saturation;
        float padding;
    };
    SDL_GPUDevice *device_;
    Uniforms uniforms_;
    bool sdr_ = false;
    SDL_GPUTexture *scene_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
    Uint32 width_ = 0;
    Uint32 height_ = 0;
};

} // namespace app
