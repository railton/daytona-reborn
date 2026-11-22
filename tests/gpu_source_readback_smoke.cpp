#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
constexpr Uint32 width = 65, height = 3;
constexpr Uint32 pitch_pixels = 128;
using Pixel = std::array<Uint8, 4>;

struct Slot {
    SDL_GPUTransferBuffer *buffer = nullptr;
    SDL_GPUFence *fence = nullptr;
};

struct Resources {
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *image = nullptr;
    std::array<Slot, 2> slots{};

    ~Resources() {
        if (device) {
            for (auto &slot : slots) {
                if (slot.fence) SDL_ReleaseGPUFence(device, slot.fence);
                if (slot.buffer) SDL_ReleaseGPUTransferBuffer(device, slot.buffer);
            }
            if (image) SDL_ReleaseGPUTexture(device, image);
            SDL_DestroyGPUDevice(device);
        }
        SDL_Quit();
    }
};

void require(bool condition) {
    if (!condition) throw std::runtime_error(SDL_GetError());
}

void submit(Resources &r, Slot &slot, const SDL_FColor &color) {
    if (slot.fence) throw std::runtime_error("readback slot still owned by a submission");
    auto *cmd = SDL_AcquireGPUCommandBuffer(r.device);
    require(cmd != nullptr);

    SDL_GPUColorTargetInfo target{};
    target.texture = r.image;
    target.clear_color = color;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    auto *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (!pass) {
        SDL_CancelGPUCommandBuffer(cmd);
        require(false);
    }
    SDL_EndGPURenderPass(pass);

    SDL_GPUTextureRegion source{};
    source.texture = r.image;
    source.w = width;
    source.h = height;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = slot.buffer;
    destination.pixels_per_row = pitch_pixels;
    destination.rows_per_layer = height;
    auto *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(cmd);
        require(false);
    }
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    slot.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    require(slot.fence != nullptr);
}

void verify(Resources &r, Slot &slot, const Pixel &expected) {
    const auto deadline = SDL_GetTicksNS() + 2'000'000'000;
    while (!SDL_QueryGPUFence(r.device, slot.fence)) {
        if (SDL_GetTicksNS() >= deadline) throw std::runtime_error("readback fence timed out");
        SDL_Delay(1);
    }
    const auto *bytes = static_cast<const Uint8 *>(SDL_MapGPUTransferBuffer(r.device, slot.buffer, false));
    require(bytes != nullptr);
    bool matches = true;
    for (Uint32 row = 0; row < height; ++row)
        for (Uint32 x = 0; x < width; ++x)
            if (std::memcmp(bytes + (row * pitch_pixels + x) * 4, expected.data(), 4) != 0)
                matches = false;
    SDL_UnmapGPUTransferBuffer(r.device, slot.buffer);
    SDL_ReleaseGPUFence(r.device, slot.fence);
    slot.fence = nullptr;
    if (!matches) throw std::runtime_error("downloaded pixels do not match generation");
}
} // namespace

int main() {
    try {
        Resources r;
        require(SDL_Init(SDL_INIT_VIDEO));
        r.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, "direct3d12");
        require(r.device != nullptr);
        SDL_GPUTextureCreateInfo image{};
        image.type = SDL_GPU_TEXTURETYPE_2D;
        image.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        image.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        image.width = width;
        image.height = height;
        image.layer_count_or_depth = 1;
        image.num_levels = 1;
        r.image = SDL_CreateGPUTexture(r.device, &image);
        require(r.image != nullptr);
        const SDL_GPUTransferBufferCreateInfo download{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                      pitch_pixels * height * 4, 0};
        for (auto &slot : r.slots) {
            slot.buffer = SDL_CreateGPUTransferBuffer(r.device, &download);
            require(slot.buffer != nullptr);
        }
        submit(r, r.slots[0], {1, 0, 0, 1});
        submit(r, r.slots[1], {0, 1, 0, 1});
        verify(r, r.slots[0], {255, 0, 0, 255});
        submit(r, r.slots[0], {0, 0, 1, 1});
        verify(r, r.slots[1], {0, 255, 0, 255});
        verify(r, r.slots[0], {0, 0, 255, 255});
        std::puts("D3D12: three generations verified with two slots and 256-byte row alignment");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "GPU readback smoke: %s\n", e.what());
        return 1;
    }
}
