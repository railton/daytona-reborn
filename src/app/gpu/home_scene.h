#pragma once

#include <SDL3/SDL_gpu.h>

namespace app {

class HomeScene {
public:
    HomeScene() = default;
    HomeScene(const HomeScene &) = delete;
    HomeScene &operator=(const HomeScene &) = delete;
    bool init(SDL_GPUDevice *dev, SDL_GPUTextureFormat swap_format);
    void shutdown();
    bool render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swap, Uint32 w, Uint32 h,
                double seconds, float visibility);

private:
    bool resize(Uint32 w, Uint32 h);
    SDL_GPUDevice *dev_ = nullptr;
    SDL_GPUGraphicsPipeline *world_ = nullptr, *lens_ = nullptr;
    SDL_GPUTexture *color_ = nullptr, *depth_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUBuffer *vertices_ = nullptr;
    Uint32 w_ = 0, h_ = 0, track_vertices_ = 0, sky_vertices_ = 0, car_vertices_ = 0;
};

} // namespace app
