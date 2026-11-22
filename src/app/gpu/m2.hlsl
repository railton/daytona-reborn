// Shaders for the hardware (SDL_GPU) renderer: one source for every backend.
// scripts/build_shaders.py compiles it with DXC to SPIR-V (Vulkan) and DXIL
// (Direct3D 12), and SPIRV-Cross turns the SPIR-V into MSL (Metal); the result
// is src/app/gpu/shaders_gen.h. Resource spaces follow SDL_GPU: vertex
// uniforms space1; fragment textures and samplers space2.

struct Screen { float2 size; float2 pad; }; // the render target, in pixels
[[vk::binding(0, 1)]] cbuffer ScreenUBO : register(b0, space1) { Screen screen; };

// Model 2 polygons, already projected to screen pixels (model2_3d_project).
// depth: the polygon's place in the hardware's draw order; the depth test keeps
// the earlier one, as the rasterizer's "first to fill a pixel wins". oz: 1/z,
// u/z, v/z at the vertex (render_one's p[0..2]), interpolated linearly in
// screen space as the rasterizer's plane equations are. The rest is the
// polygon's texture state, decoded on the CPU as render_one does:
//   info.x: renderer (bits 0-1), checker 2, wrap x 3, wrap y 4, mirror x 5,
//           mirror y 6, microtexture 7, its min LOD 8-9, sheets swapped 10,
//           max mip level 12-16, log2(width / 32) 17-19, log2(height / 32) 20-22
//   info.y: texx | texy << 16      info.z: utexx | utexy << 16
//   info.w: lumabase | luma << 16  more.x: texlod (int)  more.y: palette colour (RGB555)
struct PolyIn {
    float2 pos : TEXCOORD0;
    float depth : TEXCOORD1;
    float3 oz : TEXCOORD2;
    float4 color : TEXCOORD3;
    uint4 info : TEXCOORD4;
    uint2 more : TEXCOORD5;
};
struct PolyOut {
    float4 pos : SV_Position;
    noperspective float3 oz : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
    nointerpolation uint4 info : TEXCOORD2;
    nointerpolation uint2 more : TEXCOORD3;
};

PolyOut vs_poly(PolyIn i) {
    PolyOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, i.depth, 1.0);
    o.oz = i.oz;
    o.color = i.color;
    o.info = i.info;
    o.more = i.more;
    return o;
}

// Fragment data (read-only storage buffers, space2, as SDL_GPU orders them):
// texture RAM (sheet 0 then sheet 1, 0x80000 dwords each, as the rasterizer
// reads them), the luma RAM (one entry per dword, low byte), and the colour
// translation table with the rasterizer's gamma already applied.
[[vk::binding(0, 2)]] StructuredBuffer<uint> texram : register(t0, space2);
[[vk::binding(1, 2)]] StructuredBuffer<uint> lumaram : register(t1, space2);
[[vk::binding(2, 2)]] StructuredBuffer<uint> xlat : register(t2, space2);

[[vk::binding(3, 2)]] StructuredBuffer<uint> opponent_atlas : register(t3, space2);

float3 opponent_texel(uint offset, uint width, int2 p, int2 limit) {
    const uint c = opponent_atlas[offset + dot(uint2(clamp(p, 0, limit)), uint2(1, width))];
    return float3(c & 255u, (c >> 8u) & 255u, (c >> 16u) & 255u) / 255.0;
}

float4 opponent_paint(float2 uv, float2 footprint, uint surface, uint shade) {
    const uint2 base = uint2(256u, 128u);
    const float2 projected = footprint * float2(base);
    const uint level = uint(clamp(floor(log2(max(1.0, max(projected.x, projected.y)))), 0.0, 3.0));
    const uint2 dims = base >> level;
    const uint offset = surface * 43520u + 43690u - (43690u >> (level * 2u));
    const float2 xy = saturate(uv) * float2(dims) - 0.5;
    const int2 start = int2(floor(xy));
    const float2 weight = frac(xy);
    const int2 limit = int2(dims) - 1;
    const float3 a = lerp(opponent_texel(offset, dims.x, start, limit),
                          opponent_texel(offset, dims.x, start + int2(1, 0), limit), weight.x);
    const float3 b = lerp(opponent_texel(offset, dims.x, start + int2(0, 1), limit),
                          opponent_texel(offset, dims.x, start + int2(1, 1), limit), weight.x);
    return float4(saturate(lerp(a, b, weight.y) * (float(shade) / 127.0)), 1.0);
}

static const uint log2_table[128] = {0, 2, 5, 8, 11, 14, 16, 19, 22, 25, 27, 30, 33, 35, 38, 40, 43, 46, 48, 51, 53, 56, 58, 61, 63, 65, 68, 70, 73, 75, 77, 80, 82, 84, 87, 89, 91, 93, 96, 98, 100, 102, 104, 106, 109, 111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};

// raster.cpp fast_log2
int fast_log2(float value) {
    if (value < 0.0) return 0;
    const uint ival = asuint(value) >> 16;
    const int e = int(ival >> 7) - 127;
    return (e << 8) | int(log2_table[ival & 127u]);
}

// x86 cvttss2si: NaN and out of range give INT32_MIN (raster.cpp to_s32)
int to_s32(float f) {
    if (!(f >= -2147483648.0 && f < 2147483648.0)) return int(0x80000000u);
    return int(f);
}

uint lerp8(uint x, uint y, uint a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ffu; } // raster.cpp LERP

// raster.cpp get_texel
uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) { // sheets are mapped 2048x1024, stored 1024x2048
        x2 -= 1024;
        y2 ^= 1024;
    }
    const uint offset = uint((y2 / 2) * 512 + (x2 / 2));
    uint texel = texram[sheet * 0x80000u + ((offset >> 1) & 0x7ffffu)];
    if (offset & 1u) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0fu;
}

// raster.cpp fetch_bilinear_texel; level -1 is the microtexture
uint fetch_texel(uint4 info, int level, int u, int v, bool translucent, float2 footprint) {
    const uint flags = info.x;
    const bool swap = (flags >> 10) & 1u;
    uint tex_width, tex_height, tex_x, tex_y, sheet;
    if (level == -1) {
        tex_width = 128u;
        tex_height = 128u;
        tex_x = info.z & 0xffffu;
        tex_y = info.z >> 16;
        sheet = swap ? 0u : 1u; // texsheet[1]
        const uint sh = 1u << ((flags >> 8) & 3u);
        u <<= sh;
        v <<= sh;
        footprint *= float(1u << sh);
    } else {
        const uint texx = info.y & 0xffffu, texy = info.y >> 16;
        tex_width = (32u << ((flags >> 17) & 7u)) >> uint(level);
        tex_height = (32u << ((flags >> 20) & 7u)) >> uint(level);
        tex_x = ((texx - 2048u) >> uint(level)) & 2047u;
        tex_y = ((texy - 1024u) >> uint(level)) & 1023u;
        sheet = ((level & 1) != 0) != swap ? 1u : 0u; // texsheet[level & 1]
        u >>= level;
        v >>= level;
        footprint *= 1.0 / float(1u << level);
    }
    if (((flags >> 5) & 1u) && (u & int(tex_width << 8)) != 0) u = ~u;
    if (((flags >> 6) & 1u) && (v & int(tex_height << 8)) != 0) v = ~v;
    u -= 0x80;
    v -= 0x80;
    uint ufrac = uint(u) & 0xffu;
    uint vfrac = uint(v) & 0xffu;
    int u0 = int(uint(u >> 8) & (tex_width - 1u));
    int u1 = int(uint(u0 + 1) & (tex_width - 1u));
    int v0 = int(uint(v >> 8) & (tex_height - 1u));
    int v1 = int(uint(v0 + 1) & (tex_height - 1u));
    if (!((flags >> 3) & 1u) && u1 == 0) {
        if (ufrac >= 0x80u) { u0 = u1; u1++; ufrac = 0u; }
        else { u1 = u0; u0--; ufrac = 0x100u; }
    }
    if (!((flags >> 4) & 1u) && v1 == 0) {
        if (vfrac >= 0x80u) { v0 = 0; v1++; vfrac = 0u; }
        else { v1 = v0; v0--; vfrac = 0x100u; }
    }
    // Native mode uses screen-space derivatives to keep the blend
    // centered on the texel edge within one output pixel.
    if ((flags & (1u << 23)) != 0u) {
        const float2 blend = saturate((float2(ufrac, vfrac) - 128.0) /
                                      (256.0 * clamp(footprint, 1.0/256.0, 1.0)) + 0.5);
        ufrac = uint(blend.x * 256.0 + 0.5);
        vfrac = uint(blend.y * 256.0 + 0.5);
    }
    uint t00 = get_texel(tex_x, tex_y, u0, v0, sheet) << 4;
    uint t01 = get_texel(tex_x, tex_y, u1, v0, sheet) << 4;
    uint t10 = get_texel(tex_x, tex_y, u0, v1, sheet) << 4;
    uint t11 = get_texel(tex_x, tex_y, u1, v1, sheet) << 4;
    if (translucent) {
        if (t00 != 0xf0u) t00 |= 0x00800000u;
        if (t01 != 0xf0u) t01 |= 0x00800000u;
        if (t10 != 0xf0u) t10 |= 0x00800000u;
        if (t11 != 0xf0u) t11 |= 0x00800000u;
        if (t00 == 0xf0u) t00 = t01 & 0xffu;
        if (t01 == 0xf0u) t01 = t00 & 0xffu;
        if (t10 == 0xf0u) t10 = t11 & 0xffu;
        if (t11 == 0xf0u) t11 = t10 & 0xffu;
    }
    uint t0x = lerp8(t00, t01, ufrac);
    uint t1x = lerp8(t10, t11, ufrac);
    if (translucent) {
        if (t0x == 0xf0u) t0x = t1x & 0xffu;
        if (t1x == 0xf0u) t1x = t0x & 0xffu;
    }
    return lerp8(t0x, t1x, vfrac);
}

float4 ps_poly(PolyOut i) : SV_Target {
    const uint flags = i.info.x;
    const float z = 1.0 / i.oz.x;
    const float2 uv = i.oz.yz * z;
    // The 2x render has two raster pixels per output pixel per axis.
    const float2 footprint = 2.0 * (abs(ddx(uv)) + abs(ddy(uv)));
    if ((flags & (1u << 24)) != 0u) return opponent_paint(uv, footprint, i.info.y, i.info.z);
    if ((flags >> 2) & 1u) { // checker: the rasterizer draws pixels with (x ^ y) odd
        const uint2 p = uint2(i.pos.xy);
        if (((p.x ^ p.y) & 1u) == 0u) discard;
    }
    const uint renderer = flags & 3u;
    if (renderer < 2u) return i.color; // solid: its palette colour, from the CPU

    // raster.cpp draw_tex_span, one pixel
    const bool translucent = renderer == 3u;
    const int max_level = int((flags >> 12) & 31u);
    const int mml = -int(i.more.x) + fast_log2(z);
    const int level = clamp(mml >> 7, 0, max_level);
    const int u = to_s32(i.oz.y * z * 256.0);
    const int v = to_s32(i.oz.z * z * 256.0);
    uint t = fetch_texel(i.info, level, u, v, translucent, footprint);
    if (mml > 0 && level < max_level) {
        const uint t2 = fetch_texel(i.info, level + 1, u, v, translucent, footprint);
        t = lerp8(t, t2, uint((mml & 127) << 1));
    } else if (((flags >> 7) & 1u) && mml < 0) {
        const uint t2 = fetch_texel(i.info, -1, u, v, translucent, footprint);
        t = lerp8(t, t2, uint(min(-mml >> ((flags >> 8) & 3u), 127)));
    }
    if (translucent) {
        if (t < 0x00400000u) discard;
        t &= 0xffu;
    }
    const uint lumabase = i.info.w & 0xffffu, pluma = i.info.w >> 16;
    uint luma = ((lumaram[lumabase + (t >> 1)] & 0xffu) * pluma / 256u) & 0xffu;
    luma = min(luma, 0x3fu);
    const uint c = i.more.y;
    const uint cr = ((c >> 0) & 0x1fu) << 8, cg = 0x2000u + (((c >> 5) & 0x1fu) << 8), cb = 0x4000u + (((c >> 10) & 0x1fu) << 8);
    return float4(float(xlat[cr + luma]) / 255.0, float(xlat[cg + luma]) / 255.0, float(xlat[cb + luma]) / 255.0, 1.0);
}

// A CPU-made layer (tilemaps) as a textured quad; pixels that are 0 are holes.
struct QuadIn {
    float2 pos : TEXCOORD0;
    float2 uv : TEXCOORD1;
};
struct QuadOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// SDL_GPU's Vulkan backend takes a combined image sampler in set 2.
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> layer_tex : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState layer_smp : register(s0, space2);

QuadOut vs_quad(QuadIn i) {
    QuadOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}

float4 ps_quad(QuadOut i) : SV_Target {
    const float4 c = layer_tex.Sample(layer_smp, i.uv);
    if (all(c == 0.0)) discard;
    return float4(c.rgb, 1.0);
}

// The System 24 tilemap layers (runtime/video.cpp), composed per pixel:
// segaic24 draw_common for one layer at one screen pixel, the window masks,
// per-line scroll and the split modes included, then screen_update's order.
// Two read-only storage buffers (space2):
//   tilepix: the four layers' decoded 512x512 pixmaps, a u16 per pixel
//            (pen = colour * 16 + pixel, pixel 0 transparent; category bit 15),
//            two per word
//   tilepix after the four layers (at u16 0x100000): the matched sky
//            panorama, 4096x512 pens, 0xffff unknown (Video::sky_panorama)
//   tiledata: [0] widescreen margin, [1] its fill (0 each row's edge colours,
//            1 the sky's plain colour, 2 the backdrop stretched, 3 the sky
//            panorama, its column at x 0 in [5]), [2] the
//            integer scale; [3..4] raster viewport width and height; [16..]
//            the pens (0xAARRGGBB, 4096); [16 + 4096..] tile RAM words 0x4000 to
//            0x6fff (line scroll tables, scroll registers, window masks), two
//            per word
[[vk::binding(0, 2)]] StructuredBuffer<uint> tilepix : register(t0, space2);
[[vk::binding(1, 2)]] StructuredBuffer<uint> tiledata : register(t1, space2);

static const uint kTilePens = 16u, kTileWords = 16u + 4096u;
static const int kTileW = 496, kTileH = 384;

uint tile_word(uint i) {
    const uint j = i - 0x4000u;
    const uint d = tiledata[kTileWords + (j >> 1)];
    return (j & 1u) != 0u ? d >> 16 : d & 0xffffu;
}

uint tile_pixel(uint l, int x, int y) {
    const uint i = l * 0x40000u + uint(y & 511) * 512u + uint(x & 511);
    const uint d = tilepix[i >> 1];
    return (i & 1u) != 0u ? d >> 16 : d & 0xffffu;
}

// draw(bitmap, layer, opaque ? DRAW_OPAQUE : 0) at (x, y): true if it writes
// the pixel, with the pen it writes.
bool tile_layer(uint layer, bool opaque, int x, int y, out uint pen) {
    pen = 0u;
    const uint l = layer >> 1, tpri = layer & 1u;
    const uint hscr = tile_word(0x5000u + l);
    const uint vscr = tile_word(0x5004u + l);
    const uint ctrl = tile_word(0x5004u + (l & 2u));
    if ((vscr & 0x8000u) != 0u) return false; // layer disable

    if ((ctrl & 0x6000u) != 0u) { // special window/scroll modes: layers l and l ^ 1 split
        if ((l & 1u) != 0u) return false;
        const uint mode = (ctrl & 0x6000u) >> 13;
        const int sy = int(vscr & 0x1ffu);
        const int v = int((0u - vscr) & 0x1ffu);
        const uint lv = ((0u - vscr) & 0x200u) != 0u ? l : l ^ 1u; // mode 1: the layer above v
        uint src;
        int sx;
        if ((hscr & 0x8000u) != 0u) {
            const uint hl = tile_word(0x4000u + 0x200u * l + uint(y));
            const int h = int(hl & 0x1ffu);
            sx = -h;
            if (mode == 1u) src = y >= v ? lv ^ 1u : lv;
            else {
                const uint l1 = (hl & 0x200u) != 0u ? l : l ^ 1u;
                src = x <= h - 1 ? l1 : l1 ^ 1u;
            }
        } else {
            const int h = int(hscr & 0x1ffu);
            sx = -h;
            if (mode == 1u) src = y < v ? lv : lv ^ 1u;
            else {
                const uint lh = (hscr & 0x200u) != 0u ? l : l ^ 1u;
                src = x < h ? lh : lh ^ 1u;
            }
        }
        // tilemap_draw: the category must match, and the pixel be opaque unless drawing opaque
        const uint p = tile_pixel(src, x + sx, y + sy);
        if ((p >> 15) != tpri || (!opaque && (p & 15u) == 0u)) return false;
        pen = p & 0xfffu;
        return true;
    }

    // draw_rect: the 8-pixel window mask (inverted for odd layers), then the pixmap
    uint m = tile_word(((layer & 4u) != 0u ? 0x6800u : 0x6000u) + uint(y) * 4u + uint(x >> 7));
    if ((l & 1u) != 0u) m = ~m;
    if ((m & (0x8000u >> uint((x >> 3) & 15))) != 0u) return false;
    const int hs = (hscr & 0x8000u) != 0u ? int((0u - tile_word(0x4000u + 0x200u * l + uint(y))) & 0x1ffu)
                                          : int((0u - hscr) & 0x1ffu);
    const uint p = tile_pixel(l, x + hs, int(vscr & 0x1ffu) + y);
    if (!opaque && ((p >> 15) != tpri || (p & 15u) == 0u)) return false;
    pen = p & 0xfffu;
    return true;
}

// screen_update's back layers: 3 and 2 opaque, then 1 and 0, the last drawn
// winning; pen 0 where none draws.
uint tile_back(int x, int y) {
    uint pen;
    if (tile_layer(0u, false, x, y, pen)) return pen;
    if (tile_layer(2u, false, x, y, pen)) return pen;
    if (tile_layer(4u, true, x, y, pen)) return pen;
    if (tile_layer(6u, true, x, y, pen)) return pen;
    return 0u;
}

float4 pen_color(uint argb) {
    return float4(float((argb >> 16) & 0xffu), float((argb >> 8) & 0xffu), float(argb & 0xffu), 255.0) / 255.0;
}

int2 tile_position(float2 pixel, int margin) {
    const int2 viewport = int2(tiledata[3], tiledata[4]);
    const int2 logical = int2(kTileW + 2 * margin, kTileH);
    const int2 xy = int2(pixel) * logical / viewport;
    return xy - int2(margin, 0);
}

// The back layers, behind the 3D layer, filling the target: 496 wide in the
// centre, the widescreen margins as Video::fill_margins fills them.
float4 ps_tiles_back(QuadOut i) : SV_Target {
    const int margin = int(tiledata[0]);
    const uint fill = tiledata[1];
    const int out_w = kTileW + 2 * margin;
    const int2 xy = tile_position(i.pos.xy, margin);
    const int x = xy.x, y = xy.y;
    if (margin > 0 && fill == 2u) { // stretched: column (x + 0.5) * W / out - 0.5, blended
        const float u = clamp((float(x + margin) + 0.5) * float(kTileW) / float(out_w) - 0.5, 0.0, float(kTileW - 1));
        const int a = int(u), b = min(a + 1, kTileW - 1);
        const float f = u - float(a);
        const uint ca = tiledata[kTilePens + tile_back(a, y)], cb = tiledata[kTilePens + tile_back(b, y)];
        uint p = 0u;
        for (uint k = 0u; k < 24u; k += 8u) {
            const float c = float((ca >> k) & 0xffu) * (1.0 - f) + float((cb >> k) & 0xffu) * f;
            p |= uint(c + 0.5) << k;
        }
        return pen_color(p);
    }
    if (margin > 0 && fill == 3u && (x < 0 || x >= kTileW)) {
        // the sky panorama (Video::fill_margins): layer 2's rows above the
        // split continue it at tiledata[5]; other rows and unknown pixels
        // are the plain sky.
        const uint mode = tile_word(0x5006u);
        const int psy = int(mode & 0x1ffu), v = int((0u - mode) & 0x1ffu);
        const bool top_is_2 = ((0u - mode) & 0x200u) != 0u;
        if ((y < v) == top_is_2) {
            const uint i = 0x100000u + uint((y + psy) & 511) * 4096u + uint((x + int(tiledata[5])) & 4095);
            const uint d = tilepix[i >> 1];
            const uint pen = (i & 1u) != 0u ? d >> 16 : d & 0xffffu;
            if (pen != 0xffffu) return pen_color(tiledata[kTilePens + (pen & 0xfffu)]);
        }
        return pen_color(tiledata[kTilePens + tile_back(0, 0)]);
    }
    int sx = x, sy = y;
    if (x < 0 || x >= kTileW) {
        if (fill == 1u) sx = 0, sy = 0; // the sky: the top-left pixel
        else sx = x < 0 ? 0 : kTileW - 1;
    }
    return pen_color(tiledata[kTilePens + tile_back(sx, sy)]);
}

// The front layers, over the 3D layer: 3 to 0, the last drawn winning; holes
// where none draws.
float4 ps_tiles_front(QuadOut i) : SV_Target {
    const int2 xy = tile_position(i.pos.xy, int(tiledata[0]));
    const int x = xy.x, y = xy.y;
    if (x < 0 || x >= kTileW) discard;
    uint pen;
    for (uint l = 1u; l < 8u; l += 2u)
        if (tile_layer(l, false, x, y, pen)) return pen_color(tiledata[kTilePens + pen]);
    discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
