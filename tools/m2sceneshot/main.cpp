#include "../common/gpu_submission.h"
#include "app/gpu/m2_polygon_material.h"
#include "runtime/checked_trace_file.h"
#include "runtime/msc2_snapshot.h"
#include "runtime/msc2_frame_context.h"
#include "runtime/msc2_layer_memory.h"
#include "runtime/msc2_material_memory.h"
#include "runtime/msc2_priority.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>
#include <cstddef>
#include <span>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr int reference_width = 496, reference_height = 384, scale = 2;
constexpr Uint32 width = reference_width * scale, height = reference_height * scale;
constexpr auto colour_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
constexpr auto depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
struct Vertex { float x, y, z, u, v; };
using Uniforms = std::array<float, 24>;
static_assert(sizeof(Vertex) == 20 && sizeof(Uniforms) == 96);

std::vector<uint8_t> load(const char *path, std::optional<size_t> exact_size = std::nullopt) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("could not open input");
    const std::streamoff length = file.tellg();
    if (length < 0 || uintmax_t(length) > std::numeric_limits<size_t>::max() ||
        uintmax_t(length) > uintmax_t(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("invalid input length");
    if (exact_size && uintmax_t(length) != *exact_size)
        throw std::runtime_error("unexpected input length");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        file.peek() != std::char_traits<char>::eof() || file.bad())
        throw std::runtime_error("could not read complete input");
    return bytes;
}

rt::msc2::FrameContext load_context(const char *path) {
    const auto bytes = load(path);
    const auto context = rt::msc2::read_context(std::string(bytes.begin(), bytes.end()));
    if (context.width != reference_width || context.height != reference_height)
        throw std::runtime_error("viewer expects a 496x384 reference context");
    return context;
}

template<class T> T checked(T result, const char *operation) {
    if (!result) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    return result;
}

struct Device {
    SDL_GPUDevice *gpu = nullptr;
    SDL_GPUShader *vs = nullptr, *ps = nullptr;
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    SDL_GPUBuffer *geometry = nullptr;
    std::array<SDL_GPUBuffer *, 3> material_buffers{};
    SDL_GPUTransferBuffer *material_upload = nullptr;
    SDL_GPUTransferBuffer *upload = nullptr, *download = nullptr;
    SDL_GPUTexture *colour = nullptr, *depth = nullptr;
    SDL_GPUCommandBuffer *pending = nullptr;
    SDL_GPUFence *fence = nullptr;

    Device() = default;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    void release_draw_resources() {
        if (fence) SDL_ReleaseGPUFence(gpu, std::exchange(fence, nullptr));
        if (download) SDL_ReleaseGPUTransferBuffer(gpu, std::exchange(download, nullptr));
        if (upload) SDL_ReleaseGPUTransferBuffer(gpu, std::exchange(upload, nullptr));
        if (geometry) SDL_ReleaseGPUBuffer(gpu, std::exchange(geometry, nullptr));
        for (auto *&buffer : material_buffers) {
            if (buffer) SDL_ReleaseGPUBuffer(gpu, std::exchange(buffer, nullptr));
        }
        if (material_upload) SDL_ReleaseGPUTransferBuffer(gpu, std::exchange(material_upload, nullptr));
    }

    ~Device() {
        if (gpu) {
            if (pending) SDL_CancelGPUCommandBuffer(pending);
            SDL_WaitForGPUIdle(gpu);
            release_draw_resources();
            if (depth) SDL_ReleaseGPUTexture(gpu, depth);
            if (colour) SDL_ReleaseGPUTexture(gpu, colour);
            if (pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu, pipeline);
            if (ps) SDL_ReleaseGPUShader(gpu, ps);
            if (vs) SDL_ReleaseGPUShader(gpu, vs);
            SDL_DestroyGPUDevice(gpu);
        }
        SDL_Quit();
    }
};

// DXIL container and OSG1 layout are defined in the DirectX Shader Compiler
// include/dxc/DxilContainer/DxilContainer.h. Check the two written clip distances.
bool has_view_clip_output(std::span<const uint8_t> bytes) {
    const auto read32 = [](std::span<const uint8_t> data, size_t offset) {
        return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
               (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
    };
    constexpr size_t container_header = 32, part_header = 8;
    if (bytes.size() < container_header || std::memcmp(bytes.data(), "DXBC", 4) != 0 ||
        read32(bytes, 24) != bytes.size())
        return false;
    const size_t count = read32(bytes, 28);
    if (count > (bytes.size() - container_header) / 4)
        return false;
    const size_t table_end = container_header + count * 4;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = read32(bytes, container_header + i * 4);
        if (offset < table_end || offset > bytes.size() - part_header)
            return false;
        const size_t size = read32(bytes, offset + 4);
        if (size > bytes.size() - offset - part_header)
            return false;
        if (std::memcmp(bytes.data() + offset, "OSG1", 4) != 0)
            continue;
        constexpr size_t signature_header = 8, record_size = 32;
        const auto signature = bytes.subspan(offset + part_header, size);
        if (signature.size() < signature_header)
            return false;
        const size_t records = read32(signature, 0), start = read32(signature, 4);
        if (start < signature_header || start > signature.size() ||
            records > (signature.size() - start) / record_size)
            return false;
        for (size_t j = 0; j < records; ++j) {
            const auto field = signature.subspan(start + j * record_size, record_size);
            constexpr uint32_t clip_distance = 2, float32 = 3;
            if (read32(field, 8) == 0 && read32(field, 12) == clip_distance &&
                read32(field, 16) == float32 && (field[24] & ~field[25] & 3) == 3)
                return true;
        }
        return false;
    }
    return false;
}

SDL_GPUShader *shader(SDL_GPUDevice *gpu, const char *path, const char *entry,
                      SDL_GPUShaderStage stage, Uint32 uniforms, Uint32 storage = 0,
                      bool require_view_clip = false) {
    const auto bytes = load(path);
    if (require_view_clip && !has_view_clip_output(bytes))
        throw std::runtime_error("world view clipping requires a vertex shader with two clip distances");
    SDL_GPUShaderCreateInfo info{};
    info.code = bytes.data();
    info.code_size = bytes.size();
    info.entrypoint = entry;
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
    info.stage = stage;
    info.num_uniform_buffers = uniforms;
    info.num_storage_buffers = storage;
    return checked(SDL_CreateGPUShader(gpu, &info), "create shader");
}

SDL_GPUTexture *target(SDL_GPUDevice *gpu, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return checked(SDL_CreateGPUTexture(gpu, &info), "create texture");
}

struct Range {
    Uint32 first, count;
    size_t primitive;
    const rt::SceneCache::PrimitiveState *state;
    SDL_Rect clip;
};
struct Draw {
    Uint32 first, count;
    size_t source_primitive;
    Uniforms uniforms;
    std::array<uint32_t, 8> material{};
    SDL_Rect clip;
};
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;
    size_t skipped_primitives = 0, skipped_instances = 0, omitted_mode1 = 0;
    size_t source_primitives = 0;
};

std::optional<SDL_Rect> clip_rect(const rt::SceneCache::PrimitiveState &state,
                                 const rt::msc2::FrameContext *context) {
    if (!context) return SDL_Rect{0, 0, int(width), int(height)};
    const auto &v = state.viewport;
    const auto left = std::max<int64_t>(int64_t(v[0]) + context->render_x, 0);
    const auto right = std::min<int64_t>(int64_t(v[2]) + context->render_x, reference_width - 1);
    const auto top = std::max<int64_t>(reference_height - int64_t(v[3]) + context->render_y, 0);
    const auto bottom = std::min<int64_t>(reference_height - int64_t(v[1]) + context->render_y,
                                          reference_height - 1);
    if (left > right || top > bottom) return std::nullopt;
    return SDL_Rect{int(left) * scale, int(top) * scale,
                    int(right - left + 1) * scale, int(bottom - top + 1) * scale};
}

Uniforms uniforms(const rt::msc2::Instance &instance, const rt::SceneCache::PrimitiveState &state,
                  const rt::msc2::FrameContext *context) {
    const auto &m = instance.matrix;
    const uint32_t rgb = state.texheader[0] * 2654435761u;
    return {m[0],m[3],m[6],m[9], m[1],m[4],m[7],m[10], m[2],m[5],m[8],m[11],
            instance.focus_x, instance.focus_y,
            float(state.center[0]) + (context ? float(context->crtc_x) : 0.f),
            float(reference_height - state.center[1]) + (context ? float(context->crtc_y) : 0.f),
            float(reference_width), float(reference_height), .01f, 100000.f,
            float((rgb>>16)&255)/255.f,float((rgb>>8)&255)/255.f,float(rgb&255)/255.f,1.f};
}

uint16_t word(std::span<const uint8_t> bytes, uint32_t index) {
    return uint16_t(bytes[index * 2] | bytes[index * 2 + 1] << 8);
}

uint32_t gamma(uint8_t value) {
    return uint32_t(std::max((int(value) - 64) * 255 / 191, 0));
}

void apply_material(Draw &draw, const rt::SceneCache::PrimitiveState &state,
                    const rt::msc2::MaterialView &memory) {
    const uint32_t index = m2gpu::polygon_palette_index(state.texheader[3]) - 0x1000;
    const uint16_t colour = word(memory.palette, index);
    const auto packed = m2gpu::pack_polygon_material(
        state.texheader[0], state.texheader[1], state.texheader[2], state.luma,
        state.texlod, 128, colour); // The viewer renders at 2x.
    std::copy(packed.info.begin(), packed.info.end(), draw.material.begin());
    std::copy(packed.more.begin(), packed.more.end(), draw.material.begin() + 4);
    for (unsigned channel = 0; channel < 3; ++channel) {
        const auto base = channel * 0x2000 + (((colour >> (channel * 5)) & 31u) << 8);
        draw.uniforms[20 + channel] = gamma(uint8_t(word(memory.xlat, base + (state.luma >> 2)))) / 255.0f;
    }
}

Geometry prepare(const rt::msc2::Snapshot &scene, const rt::msc2::FrameContext *context,
                 const rt::msc2::MaterialView *material, std::span<const uint8_t> roles = {}) {
    Geometry result;
    std::unordered_map<uint32_t, const rt::msc2::Mesh *> meshes;
    std::unordered_map<uint32_t, std::vector<Range>> uploaded;
    for (const auto &mesh : scene.meshes) meshes.emplace(mesh.id, &mesh);
    constexpr size_t vertex_limit = std::numeric_limits<Uint32>::max() / sizeof(Vertex);
    size_t instance_ordinal = 0;
    for (const auto &instance : scene.instances) {
        const size_t ordinal = instance_ordinal++;
        const auto &primitives = meshes.at(instance.mesh)->primitives;
        const size_t primitive_offset = result.source_primitives;
        result.source_primitives += primitives.size();
        if (!std::all_of(instance.matrix.begin(), instance.matrix.end(),
                         [](float value) { return std::isfinite(value); }) ||
            !std::isfinite(instance.focus_x) || !std::isfinite(instance.focus_y)) {
            ++result.skipped_instances;
            continue;
        }
        auto [it, first_upload] = uploaded.try_emplace(instance.mesh);
        if (first_upload) {
            for (size_t primitive = 0; primitive < primitives.size(); ++primitive) {
                const auto &p = primitives[primitive];
                const auto clip = clip_rect(p.state, context);
                // The historical SDL GPU path omits translucent solids.
                if (material && ((p.state.texheader[0] >> 13) & 3u) == 1u) {
                    ++result.omitted_mode1;
                    continue;
                }
                if (!clip || p.vertices.size() < 3 ||
                    !std::all_of(p.vertices.begin(), p.vertices.end(), [material](const auto &v) {
                        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                               (!material || (std::isfinite(v.u) && std::isfinite(v.v)));
                    })) {
                    ++result.skipped_primitives;
                    continue;
                }
                if (p.vertices.size() - 2 > (vertex_limit - result.vertices.size()) / 3)
                    throw std::runtime_error("snapshot exceeds GPU buffer size");
                const auto first = static_cast<Uint32>(result.vertices.size());
                for (size_t k = 1; k + 1 < p.vertices.size(); ++k) {
                    for (size_t index : {size_t(0), k, k+1}) {
                        const auto &v = p.vertices[index];
                        result.vertices.push_back({v.x,v.y,v.z,v.u,v.v});
                    }
                }
                it->second.push_back({first, static_cast<Uint32>(result.vertices.size()) - first,
                                      primitive, &p.state, *clip});
            }
        }
        for (const auto &range : it->second) {
            Draw call{range.first, range.count, primitive_offset + range.primitive,
                      uniforms(instance, *range.state, context), {}, range.clip};
            if (material) apply_material(call, *range.state, *material);
            if (!roles.empty()) call.material[7] = roles[ordinal] == 0;
            result.draws.push_back(call);
        }
    }
    return result;
}

void apply_reference_priority(Geometry &scene, std::span<const uint8_t> bytes) {
    std::vector<rt::msc2::DrawPriority> keys;
    keys.reserve(scene.source_primitives);
    std::unordered_set<uint32_t> emissions;
    for (size_t i = 0; i < scene.source_primitives; ++i) {
        const auto key = rt::msc2::read_priority(bytes, i);
        if (!emissions.insert(key.emission).second)
            throw std::runtime_error("duplicate reference priority emission");
        keys.push_back(key);
    }
    std::vector<size_t> order(scene.draws.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto &first = keys[scene.draws[a].source_primitive];
        const auto &second = keys[scene.draws[b].source_primitive];
        if (first.window != second.window) return first.window > second.window;
        if (first.depth_bucket != second.depth_bucket) return first.depth_bucket < second.depth_bucket;
        return first.emission > second.emission;
    });
    for (size_t rank = 0; rank < order.size(); ++rank) {
        const float priority = 1.0f - float(rank + 1) / float(order.size() + 2);
        scene.draws[order[rank]].material[6] = std::bit_cast<uint32_t>(priority);
    }
}

void setup(Device &d, const char *vs, const char *ps, bool textured, bool require_view_clip = false) {
    checked(SDL_Init(SDL_INIT_VIDEO), "initialize SDL");
    d.gpu = checked(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, "direct3d12"), "create GPU device");
    d.vs = shader(d.gpu, vs, "vs_scene", SDL_GPU_SHADERSTAGE_VERTEX, textured ? 2 : 1, 0, require_view_clip);
    d.ps = shader(d.gpu, ps, "ps_scene", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, textured ? 3 : 0);
    const SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, u)},
    };
    SDL_GPUColorTargetDescription colour{};
    colour.format = colour_format;
    SDL_GPUGraphicsPipelineCreateInfo pipe{};
    pipe.vertex_shader = d.vs;
    pipe.fragment_shader = d.ps;
    pipe.vertex_input_state = {&buffer, 1, attributes, textured ? 2u : 1u};
    pipe.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
    pipe.depth_stencil_state.enable_depth_test = true;
    pipe.depth_stencil_state.enable_depth_write = true;
    pipe.target_info.color_target_descriptions = &colour;
    pipe.target_info.num_color_targets = 1;
    pipe.target_info.depth_stencil_format = depth_format;
    pipe.target_info.has_depth_stencil_target = true;
    d.pipeline = checked(SDL_CreateGPUGraphicsPipeline(d.gpu, &pipe), "create pipeline");
    d.colour = target(d.gpu, colour_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    d.depth = target(d.gpu, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
}

void upload(Device &d, const std::vector<Vertex> &vertices) {
    if (vertices.empty()) return;
    const auto bytes = static_cast<Uint32>(vertices.size() * sizeof(Vertex));
    SDL_GPUBufferCreateInfo buffer{};
    buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer.size = bytes;
    d.geometry = checked(SDL_CreateGPUBuffer(d.gpu, &buffer), "create vertex buffer");
    SDL_GPUTransferBufferCreateInfo transfer{};
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer.size = bytes;
    d.upload = checked(SDL_CreateGPUTransferBuffer(d.gpu, &transfer), "create upload buffer");
    auto *mapped = checked(SDL_MapGPUTransferBuffer(d.gpu, d.upload, false), "map upload");
    std::memcpy(mapped, vertices.data(), bytes);
    SDL_UnmapGPUTransferBuffer(d.gpu, d.upload);
    auto *copy = checked(SDL_BeginGPUCopyPass(d.pending), "begin upload pass");
    const SDL_GPUTransferBufferLocation source{d.upload, 0};
    const SDL_GPUBufferRegion destination{d.geometry, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
}

void upload_material(Device &d, const rt::msc2::MaterialView &material) {
    constexpr Uint32 texture_bytes = 2 * rt::msc2::material_sheet_size;
    constexpr Uint32 luma_bytes = rt::msc2::material_luma_size;
    constexpr Uint32 xlat_words = rt::msc2::material_xlat_size / 2;
    constexpr std::array<Uint32, 3> sizes{texture_bytes, luma_bytes, xlat_words * 4};
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = sizes[0] + sizes[1] + sizes[2];
    d.material_upload = checked(SDL_CreateGPUTransferBuffer(d.gpu, &info), "create material upload");
    auto *bytes = static_cast<uint8_t *>(
        checked(SDL_MapGPUTransferBuffer(d.gpu, d.material_upload, false), "map material upload"));
    std::memcpy(bytes, material.tex0.data(), material.tex0.size());
    std::memcpy(bytes + material.tex0.size(), material.tex1.data(), material.tex1.size());
    std::memcpy(bytes + texture_bytes, material.luma.data(), material.luma.size());
    auto *xlat = reinterpret_cast<uint32_t *>(bytes + texture_bytes + luma_bytes);
    for (uint32_t i = 0; i < xlat_words; ++i) xlat[i] = gamma(uint8_t(word(material.xlat, i)));
    SDL_UnmapGPUTransferBuffer(d.gpu, d.material_upload);

    for (size_t i = 0; i < sizes.size(); ++i) {
        SDL_GPUBufferCreateInfo buffer{};
        buffer.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        buffer.size = sizes[i];
        d.material_buffers[i] = checked(SDL_CreateGPUBuffer(d.gpu, &buffer), "create material buffer");
    }
    auto *pass = checked(SDL_BeginGPUCopyPass(d.pending), "begin material copy");
    Uint32 offset = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const SDL_GPUTransferBufferLocation source{d.material_upload, offset};
        const SDL_GPUBufferRegion destination{d.material_buffers[i], 0, sizes[i]};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        offset += sizes[i];
    }
    SDL_EndGPUCopyPass(pass);
}

void queue_colour_readback(SDL_GPUCommandBuffer *command, SDL_GPUTexture *colour,
                           SDL_GPUTransferBuffer *download) {
    auto *copy = checked(SDL_BeginGPUCopyPass(command), "begin download pass");
    SDL_GPUTextureRegion source{};
    source.texture = colour;
    source.w = width;
    source.h = height;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
}

// The caller owns the transfer buffer and waits for its submission's fence.
std::vector<uint8_t> read_colour(SDL_GPUDevice *gpu, SDL_GPUTransferBuffer *download,
                                 const rt::msc2::LayerView *layers = nullptr) {
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    const auto *bgra = static_cast<const uint8_t *>(
        checked(SDL_MapGPUTransferBuffer(gpu, download, false), "map readback"));
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        const uint8_t *source = bgra + i * 4;
        if (layers) {
            const size_t x = (i % width) / scale, y = (i / width) / scale;
            const size_t offset = (y * reference_width + x) * 4;
            const uint8_t *front = layers->foreground.data() + offset;
            if (front[0] || front[1] || front[2] || front[3]) source = front;
            else if (!source[3]) source = layers->background.data() + offset;
        }
        rgb[i*3] = source[2];
        rgb[i*3+1] = source[1];
        rgb[i*3+2] = source[0];
    }
    SDL_UnmapGPUTransferBuffer(gpu, download);
    return rgb;
}

void record_draws(SDL_GPUCommandBuffer *command, SDL_GPUTexture *colour_texture,
                  SDL_GPUTexture *depth_texture, SDL_GPUGraphicsPipeline *pipeline,
                  SDL_GPUBuffer *geometry, const std::vector<Draw> &draws,
                  SDL_GPUBuffer *const *material_buffers, Uint32 material_buffer_count,
                  float clear_alpha) {
    SDL_GPUColorTargetInfo colour{};
    colour.texture = colour_texture;
    // The material shader writes alpha one or discards, and this pipeline has no blending.
    colour.clear_color = {.04f,.06f,.09f,clear_alpha};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_texture;
    depth.clear_depth = 0.f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    auto *pass = checked(SDL_BeginGPURenderPass(command, &colour, 1, &depth), "begin render pass");
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (material_buffers)
        SDL_BindGPUFragmentStorageBuffers(pass, 0, material_buffers, material_buffer_count);
    if (geometry) {
        const SDL_GPUBufferBinding binding{geometry, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        for (const auto &call : draws) {
            SDL_SetGPUScissor(pass, &call.clip);
            SDL_PushGPUVertexUniformData(command, 0, call.uniforms.data(), sizeof(Uniforms));
            if (material_buffers)
                SDL_PushGPUVertexUniformData(command, 1, call.material.data(), sizeof(call.material));
            SDL_DrawGPUPrimitives(pass, call.count, 1, call.first, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
}

std::vector<uint8_t> draw(Device &d, const Geometry &scene, const rt::msc2::MaterialView *material,
                          const rt::msc2::LayerView *layers) {
    SDL_GPUTransferBufferCreateInfo transfer{};
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer.size = width * height * 4;
    d.download = checked(SDL_CreateGPUTransferBuffer(d.gpu, &transfer), "create download buffer");
    d.pending = checked(SDL_AcquireGPUCommandBuffer(d.gpu), "acquire GPU command buffer");
    upload(d, scene.vertices);
    if (material) upload_material(d, *material);

    record_draws(d.pending, d.colour, d.depth, d.pipeline, d.geometry, scene.draws,
                 material ? d.material_buffers.data() : nullptr,
                 Uint32(d.material_buffers.size()), layers ? 0.f : 1.f);

    queue_colour_readback(d.pending, d.colour, d.download);
    d.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(d.pending);
    d.pending = nullptr;
    checked(d.fence, "submit draw");
    checked(SDL_WaitForGPUFences(d.gpu, true, &d.fence, 1), "wait for readback");

    auto rgb = read_colour(d.gpu, d.download, layers);
    d.release_draw_resources();
    return rgb;
}

void verify_reference(const std::vector<uint8_t> &rgb, const char *path, const char *label) {
    const auto header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const auto expected = load(path, header.size() + rgb.size());
    if (!std::equal(header.begin(), header.end(), expected.begin()) ||
        !std::equal(rgb.begin(), rgb.end(), expected.begin() + header.size()))
        throw std::runtime_error(std::string(label) + " differs from its single-scene reference");
}

void verify_pair(char **argv) {
    const auto a = rt::msc2::read(load(argv[2]));
    const auto context_a = load_context(argv[3]);
    const auto b = rt::msc2::read(load(argv[4]));
    const auto context_b = load_context(argv[5]);
    const auto geometry_a = prepare(a, &context_a, nullptr);
    const auto geometry_b = prepare(b, &context_b, nullptr);

    Device device;
    setup(device, argv[6], argv[7], false);
    const auto first = draw(device, geometry_a, nullptr, nullptr);
    const auto second = draw(device, geometry_b, nullptr, nullptr);
    verify_reference(first, argv[8], "first scene");
    verify_reference(second, argv[9], "second scene");
    if (first == second) throw std::runtime_error("scene pair has identical images");
    if (draw(device, geometry_a, nullptr, nullptr) != first)
        throw std::runtime_error("first scene changed after drawing the second scene");
    std::cout << SDL_GetGPUDeviceDriver(device.gpu) << " A-B-A verified"
              << " first_draws=" << geometry_a.draws.size()
              << " second_draws=" << geometry_b.draws.size() << '\n';
}
// Opt-in CPU-only diagnostic: counts the vertex bytes of each draw in scene B that
// are potentially reusable because a draw of scene A holds the exact same bytes.
// Draw materials/uniforms are separate from the vertex payload and are ignored.
// Nothing here touches the GPU or the default rendering path.
struct VertexReuseCount {
    size_t reused_draws = 0, new_draws = 0, reused_bytes = 0, new_bytes = 0;
    bool operator==(const VertexReuseCount &) const = default;
};
std::span<const Vertex> draw_vertices(const Geometry &scene, const Draw &draw) {
    if (draw.first > scene.vertices.size() || draw.count > scene.vertices.size() - draw.first)
        throw std::runtime_error("draw vertex range out of bounds");
    return std::span(scene.vertices).subspan(draw.first, draw.count);
}
uint64_t vertex_reuse_key(std::span<const Vertex> data) {
    rt::msc2::SnapshotFingerprint fingerprint;
    fingerprint.append({reinterpret_cast<const uint8_t *>(data.data()), data.size_bytes()});
    return fingerprint.hash;
}
// force_collision makes every key equal, so only the size + exact byte check decides.
VertexReuseCount count_vertex_reuse(const Geometry &a, const Geometry &b, bool force_collision = false) {
    const auto key = [force_collision](std::span<const Vertex> data) {
        return force_collision ? uint64_t(0) : vertex_reuse_key(data);
    };
    std::unordered_multimap<uint64_t, size_t> prior;
    for (size_t i = 0; i < a.draws.size(); ++i) {
        const auto data = draw_vertices(a, a.draws[i]);
        if (!data.empty()) prior.emplace(key(data), i);
    }
    VertexReuseCount result;
    for (const auto &draw : b.draws) {
        const auto data = draw_vertices(b, draw);
        if (data.empty()) continue;
        const auto [first, last] = prior.equal_range(key(data));
        const bool found = std::any_of(first, last, [&](const auto &entry) {
            const auto previous = draw_vertices(a, a.draws[entry.second]);
            return data.size() == previous.size() &&
                std::memcmp(data.data(), previous.data(), data.size_bytes()) == 0;
        });
        if (found) {
            ++result.reused_draws;
            result.reused_bytes += data.size_bytes();
        } else {
            ++result.new_draws;
            result.new_bytes += data.size_bytes();
        }
    }
    return result;
}
void vertex_reuse_require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void vertex_reuse_self_test() {
    constexpr size_t tri = 3 * sizeof(Vertex);
    Geometry a, b;
    vertex_reuse_require(count_vertex_reuse(a, b) == VertexReuseCount{}, "empty scenes must count nothing");
    a.vertices = {{0,0,2,0,0}, {1,0,2,0,0}, {0,1,2,0,0},
                  {0,0,3,0,0}, {1,0,3,0,0}, {0,1,3,0,0}};
    a.draws.resize(2);
    a.draws[0].first = 0; a.draws[0].count = 3;
    a.draws[1].first = 3; a.draws[1].count = 3;
    b = a;
    for (const bool collide : {false, true}) {
        Geometry c = a;
        c.draws[0].count = 0;
        auto r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 1 && r.new_draws == 0 && r.reused_bytes == tri,
                             "empty draws must be skipped");
        Geometry none;
        r = count_vertex_reuse(none, a, collide);
        vertex_reuse_require(r.new_draws == 2 && r.new_bytes == 2 * tri && !r.reused_bytes,
                             "empty previous scene must reuse nothing");
        c = a;
        std::swap(c.draws[0], c.draws[1]);
        r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 2 && r.reused_bytes == 2 * tri && !r.new_bytes,
                             "reordered ranges must reuse both payloads");
        c = a;
        c.draws[1] = c.draws[0];
        r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 2 && r.reused_bytes == 2 * tri,
                             "duplicate draws are counted per draw");
        c = a;
        c.vertices = {a.vertices[0], a.vertices[1], a.vertices[2], a.vertices[0]};
        c.draws[0].first = 0; c.draws[0].count = 3;
        c.draws[1].first = 1; c.draws[1].count = 3;
        r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 1 && r.new_draws == 1 && r.reused_bytes == tri && r.new_bytes == tri,
                             "overlapping ranges are counted per draw by their own bytes");
        c = a;
        c.draws[0].uniforms[0] = 1.0f;
        c.draws[0].material[0] = 7;
        c.draws[1].source_primitive = 99;
        r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 2 && r.reused_bytes == 2 * tri,
                             "material/transform live outside the vertex payload");
        c = a;
        c.vertices[3].u = -0.0f;
        r = count_vertex_reuse(a, c, collide);
        vertex_reuse_require(r.reused_draws == 1 && r.new_draws == 1 && r.new_bytes == tri,
                             "different bytes (signed zero) must not match");
    }
    b.draws[1].count = 4;
    for (const bool collide : {false, true}) {
        bool rejected = false;
        try {
            count_vertex_reuse(a, b, collide);
        } catch (const std::runtime_error &error) {
            rejected = std::string_view(error.what()) == "draw vertex range out of bounds";
        }
        vertex_reuse_require(rejected, "out-of-bounds draw was accepted");
    }
    b = a;
    b.draws[1].first = std::numeric_limits<Uint32>::max();
    bool rejected = false;
    try { count_vertex_reuse(a, b); } catch (const std::runtime_error &) { rejected = true; }
    vertex_reuse_require(rejected, "out-of-range first index was accepted");
}
void vertex_reuse_diagnostic(char **argv) {
    const auto ca = load_context(argv[3]), cb = load_context(argv[5]);
    const auto a = prepare(rt::msc2::read(load(argv[2])), &ca, nullptr);
    const auto b = prepare(rt::msc2::read(load(argv[4])), &cb, nullptr);
    // Several draws can share a vertex range. These are per-draw totals, not buffer sizes.
    const auto exact = count_vertex_reuse(a, b);
    std::printf("draws %zu reused %zu new; potential bytes %zu reused %zu new (CPU diagnostic only)\n",
                exact.reused_draws, exact.new_draws, exact.reused_bytes, exact.new_bytes);
}
// ---------------------------------------------------------------------------------------
// --arena-aba: first synchronous vertex-arena batch (isolated prototype, opt-in only).
//
// One fixed-capacity GPU vertex buffer ("arena") and one fixed-capacity upload buffer keep
// the vertex bytes of EVERY scene drawn so far (A and B are both fully retained). A draw
// whose vertex payload already sits in the arena (same size, same hash, exact memcmp against
// the CPU shadow) is not uploaded again: it simply draws from the existing offset. New payloads
// are appended after the current end. There is no eviction, no compaction and no growth.
//
// Stop conditions (documented behaviour, all throw before touching the GPU or the index):
//  * capacity: a scene that does not fit in what is left of the arena aborts with
//    "vertex arena capacity exceeded"; the index is rolled back to its state before the scene.
//  * fence: every pass waits for its fence before the next pass starts. If submit or the wait
//    fails, the arena is marked unusable and every later pass throws, because the residency of
//    the bytes just appended can no longer be proven.
//
// Transfer safety: each byte of the upload buffer is written once, at the same offset as its
// arena destination, and never rewritten. So a source still in flight is never overwritten, and
// the vertex buffer is never cycled (cycle=false everywhere), so old ranges stay valid.
// Draw state (uniforms, scissor, material words) stays per draw and independent of residency.
// Textured/material scenes are out of scope of this first batch (no material buffers here).
constexpr size_t arena_capacity_vertices = 3'000'000;
constexpr size_t arena_capacity_bytes = arena_capacity_vertices * sizeof(Vertex);
static_assert(arena_capacity_bytes <= std::numeric_limits<Uint32>::max());

struct ArenaPass {
    std::vector<Draw> draws;        // same order and state as the scene, only `first` remapped
    Uint32 new_begin = 0, new_end = 0; // byte range appended by this pass (empty if begin == end)
    size_t reused_draws = 0, new_draws = 0, reused_bytes = 0, new_bytes = 0;
};

// CPU-only index; owns the shadow copy of every resident byte. No GPU access.
class VertexArenaIndex {
public:
    explicit VertexArenaIndex(size_t capacity = arena_capacity_bytes, bool force_collision = false)
        : capacity_(capacity), force_collision_(force_collision) {}

    ArenaPass plan(const Geometry &scene) {
        const size_t used_before = shadow_.size(), entries_before = entries_.size();
        try {
            ArenaPass pass;
            pass.new_begin = static_cast<Uint32>(used_before);
            pass.draws = scene.draws;
            for (auto &draw : pass.draws) {
                const auto data = draw_vertices(scene, draw); // throws on out-of-bounds range
                if (data.empty()) { draw.first = 0; continue; }
                const auto bytes = std::as_bytes(data);
                const auto key = force_collision_ ? uint64_t(0) : vertex_reuse_key(data);
                size_t found = entries_.size();
                const auto [first, last] = by_hash_.equal_range(key);
                for (auto it = first; it != last; ++it) {
                    const auto &entry = entries_[it->second];
                    if (entry.size == bytes.size() &&
                        std::memcmp(shadow_.data() + entry.offset, bytes.data(), bytes.size()) == 0) {
                        found = it->second;
                        break;
                    }
                }
                if (found != entries_.size()) {
                    ++pass.reused_draws;
                    pass.reused_bytes += bytes.size();
                } else {
                    if (bytes.size() > capacity_ - shadow_.size())
                        throw std::runtime_error("vertex arena capacity exceeded");
                    found = entries_.size();
                    entries_.push_back({key, shadow_.size(), bytes.size()});
                    by_hash_.emplace(key, found);
                    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
                    shadow_.insert(shadow_.end(), raw, raw + bytes.size());
                    ++pass.new_draws;
                    pass.new_bytes += bytes.size();
                }
                draw.first = static_cast<Uint32>(entries_[found].offset / sizeof(Vertex));
            }
            pass.new_end = static_cast<Uint32>(shadow_.size());
            return pass;
        } catch (...) {
            rollback(used_before, entries_before);
            throw;
        }
    }
    const uint8_t *bytes_at(size_t offset) const { return shadow_.data() + offset; }
    size_t used() const { return shadow_.size(); }
    size_t entries() const { return entries_.size(); }
    // The caller must have confirmed that the appended GPU copy was cancelled.
    void discard_append(size_t used, size_t entries) { rollback(used, entries); }

private:
    struct Entry { uint64_t hash; size_t offset, size; };
    void rollback(size_t used, size_t entries) {
        while (entries_.size() > entries) {
            const auto [first, last] = by_hash_.equal_range(entries_.back().hash);
            for (auto it = first; it != last; ++it)
                if (it->second == entries_.size() - 1) { by_hash_.erase(it); break; }
            entries_.pop_back();
        }
        shadow_.resize(used);
    }
    size_t capacity_;
    bool force_collision_;
    std::vector<uint8_t> shadow_;
    std::vector<Entry> entries_;
    std::unordered_multimap<uint64_t, size_t> by_hash_;
};

struct SdlSubmissionBackend {
    using Command = SDL_GPUCommandBuffer *;
    using Fence = SDL_GPUFence *;

    SDL_GPUDevice *gpu;
    bool cancel(Command command) noexcept { return SDL_CancelGPUCommandBuffer(command); }
    Fence submit(Command command) noexcept { return SDL_SubmitGPUCommandBufferAndAcquireFence(command); }
    bool wait(Fence *fences, int count, bool all) noexcept {
        return SDL_WaitForGPUFences(gpu, all, fences, static_cast<Uint32>(count));
    }
    void wait_idle() noexcept { SDL_WaitForGPUIdle(gpu); }
    void release_fence(Fence fence) noexcept { SDL_ReleaseGPUFence(gpu, fence); }
};

struct ArenaGpu {
    SDL_GPUDevice *gpu = nullptr;
    SDL_GPUBuffer *buffer = nullptr;
    SDL_GPUTransferBuffer *staging = nullptr;
    bool unusable = false;
    size_t uploaded_bytes = 0; // single counter: bytes actually passed to SDL_UploadToGPUBuffer
    explicit ArenaGpu(SDL_GPUDevice *device, bool synchronous_staging = true) : gpu(device) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = static_cast<Uint32>(arena_capacity_bytes);
        buffer = checked(SDL_CreateGPUBuffer(gpu, &info), "create vertex arena");
        if (!synchronous_staging) return;
        SDL_GPUTransferBufferCreateInfo transfer{};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer.size = static_cast<Uint32>(arena_capacity_bytes);
        staging = SDL_CreateGPUTransferBuffer(gpu, &transfer);
        if (!staging) {
            SDL_ReleaseGPUBuffer(gpu, buffer);
            buffer = nullptr;
            throw std::runtime_error(std::string("create arena upload buffer: ") + SDL_GetError());
        }
    }
    ArenaGpu(const ArenaGpu &) = delete;
    ArenaGpu &operator=(const ArenaGpu &) = delete;
    ~ArenaGpu() {
        if (gpu) SDL_WaitForGPUIdle(gpu);
        if (staging) SDL_ReleaseGPUTransferBuffer(gpu, staging);
        if (buffer) SDL_ReleaseGPUBuffer(gpu, buffer);
    }
};

// One synchronous pass: upload only the bytes new to the arena, draw, read back, wait.
std::vector<uint8_t> draw_arena(Device &d, ArenaGpu &arena, VertexArenaIndex &index,
                                const Geometry &scene, size_t &uploaded_this_pass,
                                ArenaPass *report = nullptr) {
    if (arena.unusable) throw std::runtime_error("vertex arena unusable after a failed fence");
    auto pass_plan = index.plan(scene); // may throw on capacity; index already rolled back
    arena.unusable = true;              // cleared only once this pass's fence has completed
    uploaded_this_pass = 0;

    SDL_GPUTransferBufferCreateInfo transfer{};
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer.size = width * height * 4;
    const auto release_download = [gpu = d.gpu](SDL_GPUTransferBuffer *buffer) {
        SDL_ReleaseGPUTransferBuffer(gpu, buffer);
    };
    std::unique_ptr<SDL_GPUTransferBuffer, decltype(release_download)> download(
        checked(SDL_CreateGPUTransferBuffer(d.gpu, &transfer), "create download buffer"),
        release_download);
    // The submission settles before the local readback buffer is destroyed.
    SdlSubmissionBackend backend{d.gpu};
    rt::GpuSubmission<SdlSubmissionBackend> submission(backend);
    auto *command = checked(SDL_AcquireGPUCommandBuffer(d.gpu), "acquire GPU command buffer");
    if (!submission.begin(command)) {
        SDL_CancelGPUCommandBuffer(command);
        throw std::logic_error("fresh vertex arena submission refused its command buffer");
    }

    if (pass_plan.new_end > pass_plan.new_begin) {
        const Uint32 size = pass_plan.new_end - pass_plan.new_begin;
        auto *mapped = static_cast<uint8_t *>(
            checked(SDL_MapGPUTransferBuffer(d.gpu, arena.staging, false), "map arena upload"));
        std::memcpy(mapped + pass_plan.new_begin, index.bytes_at(pass_plan.new_begin), size);
        SDL_UnmapGPUTransferBuffer(d.gpu, arena.staging);
        auto *copy = checked(SDL_BeginGPUCopyPass(command), "begin arena upload pass");
        const SDL_GPUTransferBufferLocation source{arena.staging, pass_plan.new_begin};
        const SDL_GPUBufferRegion destination{arena.buffer, pass_plan.new_begin, size};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        uploaded_this_pass += size;
        arena.uploaded_bytes += size;
        SDL_EndGPUCopyPass(copy);
    }

    record_draws(command, d.colour, d.depth, d.pipeline, arena.buffer, pass_plan.draws,
                 nullptr, 0, 1.f);

    queue_colour_readback(command, d.colour, download.get());
    checked(submission.submit(), "submit arena draw");
    checked(submission.wait(), "wait for arena readback");
    auto rgb = read_colour(d.gpu, download.get());
    checked(submission.settle(), "settle arena draw");
    arena.unusable = false;
    if (report) *report = std::move(pass_plan);
    return rgb;
}

void arena_require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

// CPU-only checks of residency, collisions, capacity rollback and draw-state independence.
void vertex_arena_self_test() {
    constexpr size_t tri = 3 * sizeof(Vertex);
    Geometry a, b;
    a.vertices = {{0,0,2,0,0}, {1,0,2,0,0}, {0,1,2,0,0}, {0,0,3,0,0}, {1,0,3,0,0}, {0,1,3,0,0}};
    a.draws.resize(2);
    a.draws[0].first = 0; a.draws[0].count = 3;
    a.draws[1].first = 3; a.draws[1].count = 3;
    for (const bool collide : {false, true}) {
        VertexArenaIndex index(arena_capacity_bytes, collide);
        auto pa = index.plan(a);
        arena_require(pa.new_draws == 2 && pa.new_bytes == 2 * tri && pa.new_begin == 0 &&
                      pa.new_end == 2 * tri && pa.draws[1].first == 3, "first scene is all new");
        b = a;
        b.vertices[3].u = -0.0f; // signed zero differs bytewise: must not match
        b.draws[0].uniforms[0] = 5.0f;
        b.draws[0].material[0] = 7;
        auto pb = index.plan(b);
        arena_require(pb.reused_draws == 1 && pb.new_draws == 1 && pb.new_bytes == tri &&
                      pb.new_begin == 2 * tri && pb.new_end == 3 * tri, "B uploads only its difference");
        arena_require(pb.draws[0].uniforms[0] == 5.0f && pb.draws[0].material[0] == 7,
                      "draw state must stay per draw");
        auto pa2 = index.plan(a);
        arena_require(pa2.new_bytes == 0 && pa2.reused_draws == 2 && pa2.new_begin == pa2.new_end &&
                      pa2.draws[0].first == pa.draws[0].first && pa2.draws[1].first == pa.draws[1].first,
                      "last A must be fully resident, same offsets");
    }
    VertexArenaIndex small(2 * tri);
    small.plan(a);
    bool rejected = false;
    try { small.plan(b); } catch (const std::runtime_error &error) {
        rejected = std::string_view(error.what()) == "vertex arena capacity exceeded";
    }
    arena_require(rejected && small.used() == 2 * tri && small.entries() == 2,
                  "capacity overflow must throw and roll back");
    arena_require(small.plan(a).new_bytes == 0, "index still valid after rollback");
    b = a;
    b.draws[1].count = 4;
    rejected = false;
    try { small.plan(b); } catch (const std::runtime_error &) { rejected = true; }
    arena_require(rejected && small.used() == 2 * tri, "out-of-bounds draw must throw cleanly");
}

void arena_aba(char **argv) {
    const auto a = rt::msc2::read(load(argv[2]));
    const auto context_a = load_context(argv[3]);
    const auto b = rt::msc2::read(load(argv[4]));
    const auto context_b = load_context(argv[5]);
    const auto geometry_a = prepare(a, &context_a, nullptr);
    const auto geometry_b = prepare(b, &context_b, nullptr);

    Device device;
    setup(device, argv[6], argv[7], false);
    ArenaGpu arena(device.gpu);
    VertexArenaIndex index;
    size_t up_a = 0, up_b = 0, up_a2 = 0;
    ArenaPass plan_a, plan_b, plan_a2;
    const auto first = draw_arena(device, arena, index, geometry_a, up_a, &plan_a);
    const auto second = draw_arena(device, arena, index, geometry_b, up_b, &plan_b);
    verify_reference(first, argv[8], "arena first scene");
    verify_reference(second, argv[9], "arena second scene");
    if (first == second) throw std::runtime_error("scene pair has identical images");
    const auto third = draw_arena(device, arena, index, geometry_a, up_a2, &plan_a2);
    if (third != first) throw std::runtime_error("arena last A differs from first A");
    verify_reference(third, argv[8], "arena last A");
    if (up_a != plan_a.new_bytes || up_b != plan_b.new_bytes || up_a2 != 0 ||
        arena.uploaded_bytes != up_a + up_b + up_a2 || arena.uploaded_bytes != index.used())
        throw std::runtime_error("uploaded byte counter does not match the arena index");
    std::cout << SDL_GetGPUDeviceDriver(device.gpu) << " arena A-B-A verified"
              << " uploaded_bytes A=" << up_a << " B=" << up_b << " A2=" << up_a2
              << " total=" << arena.uploaded_bytes
              << " resident=" << index.used() << "/" << arena_capacity_bytes
              << " reused_draws B=" << plan_b.reused_draws << " new_draws B=" << plan_b.new_draws
              << " draws A=" << geometry_a.draws.size() << " B=" << geometry_b.draws.size() << '\n';
}
} // namespace

namespace {

enum class QueueFault { None, AbortB, CancelB, FenceA, FenceB };
struct RecordedAbort {};

struct QueueReport {
    std::array<size_t, 2> uploaded{};
    size_t bytes_after_a = 0;
    int submits = 0, waits = 0, submits_at_first_wait = 0, first_wait_fences = 0, idle_fallbacks = 0;
    bool fault_injected = false;
};

struct QueueBackend : SdlSubmissionBackend {
    QueueReport &report;
    QueueFault fault;
    Fence hidden_fence = nullptr;

    bool cancel(Command command) noexcept {
        const bool cancelled = SdlSubmissionBackend::cancel(command);
        if (cancelled && fault == QueueFault::CancelB) {
            report.fault_injected = true;
            return false;
        }
        return cancelled;
    }

    Fence submit(Command command) noexcept {
        ++report.submits;
        auto *fence = SdlSubmissionBackend::submit(command);
        if (fence && ((fault == QueueFault::FenceA && report.submits == 1) ||
                      (fault == QueueFault::FenceB && report.submits == 2))) {
            report.fault_injected = true;
            hidden_fence = fence;
            SDL_SetError("injected missing fence");
            return nullptr;
        }
        return fence;
    }

    bool wait(Fence *fences, int count, bool all) noexcept {
        if (report.waits++ == 0) {
            report.submits_at_first_wait = report.submits;
            report.first_wait_fences = count;
        }
        return SdlSubmissionBackend::wait(fences, count, all);
    }

    void wait_idle() noexcept {
        ++report.idle_fallbacks;
        SdlSubmissionBackend::wait_idle();
        if (hidden_fence) release_fence(std::exchange(hidden_fence, nullptr));
    }
};

struct QueueTargets {
    SDL_GPUTexture *colour = nullptr, *depth = nullptr;
    SDL_GPUTransferBuffer *download = nullptr;
};

struct QueuePool {
    SDL_GPUDevice *gpu;
    std::array<QueueTargets, 2> targets{};
    size_t allocations = 0;
    bool dirty = false;

    explicit QueuePool(SDL_GPUDevice *device) : gpu(device) {}
    QueuePool(const QueuePool &) = delete;
    QueuePool &operator=(const QueuePool &) = delete;

    void prepare() {
        arena_require(!dirty, "queued targets are still owned by a previous pair");
        for (auto &slot : targets) {
            if (slot.download) continue;
            if (!slot.colour) {
                slot.colour = target(gpu, colour_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
                ++allocations;
            }
            if (!slot.depth) {
                slot.depth = target(gpu, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
                ++allocations;
            }
            SDL_GPUTransferBufferCreateInfo download{};
            download.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            download.size = width * height * 4;
            slot.download = checked(SDL_CreateGPUTransferBuffer(gpu, &download), "create queued readback");
            ++allocations;
        }
        arena_require(targets[0].colour != targets[1].colour &&
                      targets[0].depth != targets[1].depth &&
                      targets[0].download != targets[1].download,
                      "queued passes need distinct targets");
    }

    ~QueuePool() {
        if (dirty) SDL_WaitForGPUIdle(gpu);
        for (const auto &slot : targets) {
            if (slot.download) SDL_ReleaseGPUTransferBuffer(gpu, slot.download);
            if (slot.depth) SDL_ReleaseGPUTexture(gpu, slot.depth);
            if (slot.colour) SDL_ReleaseGPUTexture(gpu, slot.colour);
        }
    }
};

struct QueuedPass {
    SDL_GPUDevice *gpu;
    const QueueTargets &targets;
    SDL_GPUTransferBuffer *staging = nullptr;
    rt::GpuSubmission<QueueBackend> submission;

    QueuedPass(QueueBackend &backend, const QueueTargets &slot)
        : gpu(backend.gpu), targets(slot), submission(backend) {}
    ~QueuedPass() {
        submission.settle();
        if (staging) SDL_ReleaseGPUTransferBuffer(gpu, staging);
    }
};

size_t queue_pass(Device &d, ArenaGpu &arena, VertexArenaIndex &index, const Geometry &scene,
                  QueuedPass &pass, bool abort_before_submit) {
    const size_t used = index.used(), entries = index.entries();
    const auto plan = index.plan(scene);
    const Uint32 bytes = plan.new_end - plan.new_begin;
    try {
        auto *cmd = checked(SDL_AcquireGPUCommandBuffer(d.gpu), "acquire queued command");
        if (!pass.submission.begin(cmd)) {
            if (!SDL_CancelGPUCommandBuffer(cmd)) {
                arena.unusable = true;
                SDL_WaitForGPUIdle(d.gpu);
            }
            throw std::logic_error("fresh queued submission refused its command");
        }
        if (bytes) {
            SDL_GPUTransferBufferCreateInfo upload{};
            upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            upload.size = bytes;
            pass.staging = checked(SDL_CreateGPUTransferBuffer(d.gpu, &upload), "create queued upload");
            auto *mapped = checked(SDL_MapGPUTransferBuffer(d.gpu, pass.staging, false), "map queued upload");
            std::memcpy(mapped, index.bytes_at(plan.new_begin), bytes);
            SDL_UnmapGPUTransferBuffer(d.gpu, pass.staging);
            auto *copy = checked(SDL_BeginGPUCopyPass(cmd), "begin queued upload");
            const SDL_GPUTransferBufferLocation source{pass.staging, 0};
            const SDL_GPUBufferRegion destination{arena.buffer, plan.new_begin, bytes};
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            SDL_EndGPUCopyPass(copy);
        }
        record_draws(cmd, pass.targets.colour, pass.targets.depth, d.pipeline,
                     arena.buffer, plan.draws, nullptr, 0, 1.f);
        queue_colour_readback(cmd, pass.targets.colour, pass.targets.download);
        if (abort_before_submit) throw RecordedAbort{};
    } catch (...) {
        if (!pass.submission.settle()) arena.unusable = true;
        if (!arena.unusable) index.discard_append(used, entries);
        throw;
    }
    if (!pass.submission.submit()) {
        arena.unusable = true;
        checked(false, "submit queued draw");
    }
    arena.uploaded_bytes += bytes;
    return bytes;
}

using QueuedImages = std::array<std::vector<uint8_t>, 2>;

QueuedImages queue_pair(Device &d, ArenaGpu &arena, VertexArenaIndex &index, QueuePool &pool,
                        const Geometry &a, const Geometry &b, QueueReport &report,
                        QueueFault fault = QueueFault::None) {
    if (arena.unusable) throw std::runtime_error("queued arena is unusable");
    pool.prepare();
    pool.dirty = true;
    QueueBackend backend{{d.gpu}, report, fault};
    QueuedPass first(backend, pool.targets[0]), second(backend, pool.targets[1]);
    try {
        report.uploaded[0] = queue_pass(d, arena, index, a, first, false);
        report.bytes_after_a = index.used();
        report.uploaded[1] = queue_pass(d, arena, index, b, second,
                                        fault == QueueFault::AbortB || fault == QueueFault::CancelB);
        checked(rt::wait_group(first.submission, second.submission), "wait for queued pair");
    } catch (...) {
        // Settle B first, then A, and keep the arena unusable after any uncertainty.
        if (!second.submission.settle()) arena.unusable = true;
        if (!first.submission.settle()) arena.unusable = true;
        if (!arena.unusable) pool.dirty = false;
        throw;
    }
    arena_require(report.submits == 2 && report.waits == 1 &&
                  report.submits_at_first_wait == 2 && report.first_wait_fences == 2,
                  "expected two CPU submits followed by one group wait");
    QueuedImages images{read_colour(d.gpu, first.targets.download), read_colour(d.gpu, second.targets.download)};
    // Both reads have unmapped their transfer buffers before the next pair can start.
    pool.dirty = false;
    return images;
}

void queued_pair_check(char **argv, bool abort_b, const char *fault_name = nullptr) {
    const auto context_a = load_context(argv[3]), context_b = load_context(argv[5]);
    const auto a = prepare(rt::msc2::read(load(argv[2])), &context_a, nullptr);
    const auto b = prepare(rt::msc2::read(load(argv[4])), &context_b, nullptr);
    Device device;
    setup(device, argv[6], argv[7], false);
    ArenaGpu arena(device.gpu, false);
    QueuePool pool(device.gpu);
    VertexArenaIndex index;
    QueueReport report;
    if (fault_name) {
        const std::string_view name(fault_name);
        const auto fault = name == "cancel-b" ? QueueFault::CancelB
                         : name == "fence-a" ? QueueFault::FenceA : QueueFault::FenceB;
        try {
            queue_pair(device, arena, index, pool, a, b, report, fault);
        } catch (const RecordedAbort &) {
            if (fault != QueueFault::CancelB) throw;
        } catch (const std::runtime_error &) {
            if (fault == QueueFault::CancelB || !report.fault_injected) throw;
        }
        arena_require(report.fault_injected && arena.unusable && report.idle_fallbacks == 1,
                      "expected injected uncertainty to disable the arena");
        QueueReport next;
        try {
            queue_pair(device, arena, index, pool, a, b, next);
        } catch (const std::runtime_error &error) {
            if (std::string_view(error.what()) != "queued arena is unusable" || next.submits) throw;
            std::cout << SDL_GetGPUDeviceDriver(device.gpu) << " fault=" << name
                      << " submits=" << report.submits << " idle_fallbacks=" << report.idle_fallbacks
                      << " index_bytes=" << index.used() << " reuse_refused=1\n";
            return;
        }
        throw std::runtime_error("uncertain arena accepted reuse");
    }

    size_t bytes_after_abort = 0;
    if (abort_b) {
        try {
            queue_pair(device, arena, index, pool, a, b, report, QueueFault::AbortB);
            throw std::runtime_error("expected cancellation before submitting B");
        } catch (const RecordedAbort &) {
            arena_require(!arena.unusable && report.submits == 1 && report.waits == 1 &&
                          report.first_wait_fences == 1 && index.used() == report.bytes_after_a &&
                          index.used() == arena.uploaded_bytes && !pool.dirty && pool.allocations == 6,
                          "expected confirmed cancellation of B and retention of A");
            bytes_after_abort = index.used();
        }
        report = {};
    }
    const auto images = queue_pair(device, arena, index, pool, a, b, report);
    verify_reference(images[0], argv[8], "two-in-flight scene A");
    verify_reference(images[1], argv[9], "two-in-flight scene B");
    arena_require(images[0] != images[1], "scene pair has identical images");
    QueueReport reverse;
    const auto reversed = queue_pair(device, arena, index, pool, b, a, reverse);
    const size_t reverse_bytes = reverse.uploaded[0] + reverse.uploaded[1];
    arena_require(!reverse_bytes && reversed[0] == images[1] && reversed[1] == images[0] &&
                  index.used() == arena.uploaded_bytes, "reversed pair did not reuse the arena");
    arena_require(!pool.dirty && pool.allocations == 6, "queued pair reallocated its targets");
    std::cout << SDL_GetGPUDeviceDriver(device.gpu) << " two-in-flight A-B verified"
              << (abort_b ? " after-injected-B-failure" : "") << " events=submit A,submit B,wait A+B"
              << " uploaded A=" << report.uploaded[0] << " B=" << report.uploaded[1]
              << " round2=" << reverse_bytes << " aborted_used_after_a=" << bytes_after_abort
              << " target_allocs=" << pool.allocations << '\n';
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc >= 2) {
            const std::string_view option(argv[1]);
            if (option == "--two-in-flight-aba" || option == "--two-in-flight-abort-b") {
                if (argc != 10) {
                    std::cerr << "usage: m2sceneshot " << option << " A A.context B B.context VS PS A.ppm B.ppm\n";
                    return 2;
                }
                queued_pair_check(argv, option == "--two-in-flight-abort-b");
                return 0;
            }
            if (option == "--two-in-flight-fault") {
                if (argc != 9 || (std::strcmp(argv[2], "cancel-b") && std::strcmp(argv[2], "fence-a") &&
                                  std::strcmp(argv[2], "fence-b"))) {
                    std::cerr << "usage: m2sceneshot --two-in-flight-fault CASE A A.context B B.context VS PS\n";
                    return 2;
                }
                queued_pair_check(argv + 1, false, argv[2]);
                return 0;
            }
        }
        if (argc >= 2 && std::strcmp(argv[1], "--verify-pair") == 0) {
            if (argc != 10) {
                std::cerr << "usage: m2sceneshot --verify-pair A CONTEXT_A B CONTEXT_B VS.dxil PS.dxil A.ppm B.ppm\n";
                return 2;
            }
            verify_pair(argv);
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--self-test-vertex-reuse") == 0) {
            if (argc != 2) {
                std::cerr << "usage: m2sceneshot --self-test-vertex-reuse\n";
                return 2;
            }
            vertex_reuse_self_test();
            std::puts("vertex reuse self-test passed");
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--self-test-vertex-arena") == 0) {
            if (argc != 2) {
                std::cerr << "usage: m2sceneshot --self-test-vertex-arena\n";
                return 2;
            }
            vertex_arena_self_test();
            std::puts("vertex arena self-test passed");
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--arena-aba") == 0) {
            if (argc != 10) {
                std::cerr << "usage: m2sceneshot --arena-aba A CONTEXT_A B CONTEXT_B VS.dxil PS.dxil A.ppm B.ppm\n";
                return 2;
            }
            arena_aba(argv);
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--vertex-reuse-diagnostic") == 0) {
            if (argc != 6) {
                std::cerr << "usage: m2sceneshot --vertex-reuse-diagnostic A CONTEXT_A B CONTEXT_B\n";
                return 2;
            }
            vertex_reuse_diagnostic(argv);
            return 0;
        }
        if (argc < 5 || argc > 10) {
            std::cerr << "usage: m2sceneshot SNAPSHOT VS.dxil PS.dxil OUTPUT.ppm [CONTEXT [MATERIAL [LAYERS [PRIORITY [ROLES]]]]]\n";
            return 2;
        }
        const auto snapshot_bytes = load(argv[1]);
        const auto scene = rt::msc2::read(snapshot_bytes);
        std::optional<rt::msc2::FrameContext> context;
        if (argc >= 6) context = load_context(argv[5]);
        std::vector<uint8_t> material_bytes;
        std::optional<rt::msc2::MaterialView> material;
        if (argc >= 7) {
            material_bytes = load(argv[6], rt::msc2::material_file_size);
            material = rt::msc2::read_material(material_bytes);
            rt::msc2::verify_material_snapshot(*material, snapshot_bytes);
            if (material->frame != context->frame)
                throw std::runtime_error("material frame does not match context");
        }
        std::vector<uint8_t> layer_bytes;
        std::optional<rt::msc2::LayerView> layers;
        if (argc >= 8) {
            layer_bytes = load(argv[7], rt::msc2::layer_file_size);
            layers = rt::msc2::read_layers(layer_bytes);
            rt::msc2::verify_companion_snapshot(layers->header.snapshot, snapshot_bytes);
            if (layers->header.frame != context->frame)
                throw std::runtime_error("layer frame does not match context");
        }
        const auto *memory = material ? &*material : nullptr;
        std::vector<uint8_t> role_bytes;
        std::span<const uint8_t> roles;
        if (argc == 10) {
            constexpr auto header_size = rt::msc2::companion_header_size;
            if (scene.instances.size() > std::numeric_limits<size_t>::max() - header_size)
                throw std::length_error("role companion is too large");
            const size_t expected_size = header_size + scene.instances.size();
            role_bytes = load(argv[9], expected_size);
            if (role_bytes.size() != expected_size) throw std::runtime_error("invalid role companion size");
            const auto role_header = rt::msc2::read_companion_header(role_bytes, 0x4c52324d);
            rt::msc2::verify_companion_snapshot(role_header.snapshot, snapshot_bytes);
            if (role_header.frame != context->frame)
                throw std::runtime_error("role frame does not match context");
            roles = std::span<const uint8_t>(role_bytes).subspan(header_size);
            if (std::any_of(roles.begin(), roles.end(), [](uint8_t role) { return role > 1; }))
                throw std::runtime_error("unresolved instance role");
            // This private format covers the three observed minimap producers.
            if (std::count(roles.begin(), roles.end(), uint8_t{1}) != 3)
                throw std::runtime_error("incomplete minimap role evidence");
        }
        auto geometry = prepare(scene, context ? &*context : nullptr, memory, roles);
        if (argc >= 9) {
            constexpr auto header_size = rt::msc2::companion_header_size;
            constexpr auto record_size = rt::msc2::priority_record_size;
            if (geometry.source_primitives > (std::numeric_limits<size_t>::max() - header_size) / record_size)
                throw std::length_error("reference priority input exceeds addressable size");
            const auto bytes = load(argv[8], header_size + geometry.source_primitives * record_size);
            const auto header = rt::msc2::read_companion_header(bytes, rt::msc2::priority_magic);
            rt::msc2::verify_companion_snapshot(header.snapshot, snapshot_bytes);
            if (header.frame != context->frame) throw std::runtime_error("priority frame does not match context");
            apply_reference_priority(geometry, bytes);
        }
        Device device;
        setup(device, argv[2], argv[3], material.has_value(), !roles.empty());
        const auto rgb = draw(device, geometry, memory, layers ? &*layers : nullptr);
        rt::CheckedTraceFile output(argv[4]);
        if (std::fprintf(output.get(), "P6\n%u %u\n255\n", width, height) < 0 ||
            std::fwrite(rgb.data(), 1, rgb.size(), output.get()) != rgb.size() ||
            !output.close())
            throw std::runtime_error("could not write image");
        std::cout << SDL_GetGPUDeviceDriver(device.gpu)
                  << " " << width << "x" << height
                  << " meshes=" << scene.meshes.size() << " instances=" << scene.instances.size()
                  << " uploaded_vertices=" << geometry.vertices.size() << " draws=" << geometry.draws.size()
                  << " skipped_primitives=" << geometry.skipped_primitives
                  << " skipped_instances=" << geometry.skipped_instances;
        if (context) std::cout << " context_frame=" << context->frame;
        if (material) std::cout << " textured omitted_mode1=" << geometry.omitted_mode1;
        if (layers) std::cout << " layered";
        if (argc >= 9) std::cout << " reference-priority";
        if (!roles.empty()) std::cout << " world-view-clipping";
        std::cout << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
