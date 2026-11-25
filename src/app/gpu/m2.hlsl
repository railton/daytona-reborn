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
//           max mip level 12-16, log2(width / 32) 17-19, log2(height / 32) 20-22,
//           native resolution 23, opponent paint 24; enhancements: alpha to
//           coverage (MSAA) 25, log2 of the anisotropic filtering 26-28, a
//           car's smooth light 29, a car's paint reflections 30, a car's
//           sharp magnified texels 31
//   info.y: texx | texy << 16      info.z: utexx | utexy << 16
//   info.w: lumabase | luma << 16  more.x: texlod (int)  more.y: palette colour (RGB555)
struct PolyIn {
    float2 pos : TEXCOORD0;
    float depth : TEXCOORD1;
    float3 oz : TEXCOORD2;
    float4 color : TEXCOORD3;
    uint4 info : TEXCOORD4;
    uint2 more : TEXCOORD5;
    // the cars' shading (GpuRenderer::shade_cars), each times 1/z: the
    // corner's light (0..255), normal and camera-space position
    float light_oz : TEXCOORD6;
    float3 normal_oz : TEXCOORD7;
    float3 position_oz : TEXCOORD8;
};
struct PolyOut {
    float4 pos : SV_Position;
    // centroid: with MSAA a pixel on a polygon's edge reads its texture inside
    // the polygon, not past it (without MSAA the centroid is the centre)
    noperspective centroid float3 oz : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
    nointerpolation uint4 info : TEXCOORD2;
    nointerpolation uint2 more : TEXCOORD3;
    noperspective centroid float light_oz : TEXCOORD4;
    noperspective centroid float3 normal_oz : TEXCOORD5;
    noperspective centroid float3 position_oz : TEXCOORD6;
};

// The cars' reflections and shadows (GpuRenderer::render): the direction
// towards the game's sun, camera space (y up); the sun's view of the cars
// (shadow map): its axes, the map's corner and scale in them, its depth
// range, the light left in shadow (0: no shadows) and a texel.
struct CarLight {
    float4 sun;
    float4 right, up, dir;     // the sun's view: x, y across, z along the light
    float4 map;                // x0, y0, 1 / width, 1 / height (in the sun's x, y)
    float4 depth;              // z0, 1 / depth range, shadow strength, 1 / map size
    float4 sky;                // the game's sky in sky_env: the camera axis (u, v), focal length / size (u, v); 0: none
    float4 map2;               // the far cascade (the map's right half): as map; z 0: no caster there
    float4 depth2;             // its z0, 1 / depth range
};
[[vk::binding(0, 3)]] cbuffer CarLightUBO : register(b0, space3) { CarLight car_light; };

PolyOut vs_poly(PolyIn i) {
    PolyOut o;
    o.pos = float4(i.pos.x / screen.size.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.size.y * 2.0, i.depth, 1.0);
    o.oz = i.oz;
    o.color = i.color;
    o.info = i.info;
    o.more = i.more;
    o.light_oz = i.light_oz;
    o.normal_oz = i.normal_oz;
    o.position_oz = i.position_oz;
    return o;
}

// Fragment data (read-only storage buffers, space2, as SDL_GPU orders them):
// texture RAM (sheet 0 then sheet 1, 0x80000 dwords each, as the rasterizer
// reads them), the luma RAM (one entry per dword, low byte), and the colour
// translation table with the rasterizer's gamma already applied.
// ps_poly also samples the cars' shadow map and the sky the paint reflects
// (first: SDL_GPU puts the sampled textures before the storage buffers)
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float> car_shadow : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState car_shadow_smp : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] Texture2D<float4> sky_env : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState sky_env_smp : register(s1, space2);
[[vk::binding(2, 2)]] StructuredBuffer<uint> texram : register(t2, space2);
[[vk::binding(3, 2)]] StructuredBuffer<uint> lumaram : register(t3, space2);
[[vk::binding(4, 2)]] StructuredBuffer<uint> xlat : register(t4, space2);

[[vk::binding(5, 2)]] StructuredBuffer<uint> opponent_atlas : register(t5, space2);

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
    // Sharp magnified texels (an enhancement, info.x bit 31, opaque): where
    // the four texels hold only two values (a number's or a logo's edge on
    // its background) and the texture is magnified, the bilinear blend is
    // thresholded at its middle, softened over about one output pixel: the
    // edge follows the bilinear contour, crisp and rounded, instead of a
    // blur. Elsewhere the game's blend.
    if ((flags >> 31) != 0u && !translucent && max(footprint.x, footprint.y) < 1.0) {
        const uint lo = min(min(t00, t01), min(t10, t11)), hi = max(max(t00, t01), max(t10, t11));
        const bool two = (t00 == lo || t00 == hi) && (t01 == lo || t01 == hi) && (t10 == lo || t10 == hi) &&
                         (t11 == lo || t11 == hi);
        if (two && hi > lo) {
            const float2 f = float2(ufrac, vfrac) / 256.0;
            const float b = lerp(lerp(float(t00), float(t01), f.x), lerp(float(t10), float(t11), f.x), f.y);
            const float k = clamp(max(footprint.x, footprint.y) * 0.25, 0.02, 0.5);
            const float w = smoothstep(0.5 - k, 0.5 + k, (b - float(lo)) / float(hi - lo));
            return uint(float(lo) + float(hi - lo) * w + 0.5) & 0xffu;
        }
    }
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

// raster.cpp draw_tex_span's texel at (u, v) (8 fractional bits) for the
// level of detail mml (7 fractional bits): the mip levels' blend, or the
// microtexture's; lerp8's two lanes (index, opacity)
uint sample_tex(uint4 info, int mml, int u, int v, bool translucent, float2 footprint) {
    const int max_level = int((info.x >> 12) & 31u);
    const int level = clamp(mml >> 7, 0, max_level);
    uint t = fetch_texel(info, level, u, v, translucent, footprint);
    if (mml > 0 && level < max_level) {
        const uint t2 = fetch_texel(info, level + 1, u, v, translucent, footprint);
        t = lerp8(t, t2, uint((mml & 127) << 1));
    } else if (((info.x >> 7) & 1u) && mml < 0) {
        const uint t2 = fetch_texel(info, -1, u, v, translucent, footprint);
        t = lerp8(t, t2, uint(min(-mml >> ((info.x >> 8) & 3u), 127)));
    }
    return t;
}

// A car's paint (an enhancement, info.x bit 30): the sky and the ground
// reflected, more at grazing angles (Schlick's Fresnel), and the sun as a
// highlight, from the corners' normals blended across the face.
float3 car_paint(float3 base, PolyOut i) {
    const float3 q = i.position_oz / i.oz.x;
    const float3 p = float3(q.xy, abs(q.z)); // z is negative on a car (no shadows on it)
    const float3 n = normalize(i.normal_oz);
    const float3 view = normalize(p); // from the eye to the point
    const float3 r = reflect(view, n);
    const float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(n, -view)), 5.0);
    float3 zenith = float3(0.30, 0.50, 0.92), horizon = float3(0.82, 0.87, 0.95);
    const float3 ground = float3(0.30, 0.30, 0.32);
    float3 env;
    if (car_light.sky.z > 0.0) {
        // the course's own sky, as the camera sees it this frame (the back
        // tilemap layers): the reflected ray projected by the camera, rays
        // going back mirrored forward, never below the horizon row; blurred
        // (a finer mip head on, where the paint is a sharper mirror)
        const float3 d = float3(r.x, max(r.y, 0.0), max(abs(r.z), 0.35));
        float2 uv = car_light.sky.xy + float2(d.x, -d.y) / d.z * car_light.sky.zw;
        uv = float2(saturate(uv.x), clamp(uv.y, 0.0, car_light.sky.y));
        const float blur = lerp(3.5, 2.0, fresnel);
        const float3 sky = sky_env.SampleLevel(sky_env_smp, uv, blur).rgb;
        horizon = sky_env.SampleLevel(sky_env_smp, float2(uv.x, car_light.sky.y), 4.0).rgb;
        env = r.y >= 0.0 ? sky : lerp(horizon * 0.8, ground, saturate(-r.y * 4.0));
    } else {
        env = r.y >= 0.0 ? lerp(horizon, zenith, sqrt(saturate(r.y)))
                         : lerp(horizon * 0.8, ground, saturate(-r.y * 4.0));
    }
    float3 c = lerp(base, env, 0.02 + 0.6 * fresnel); // little head on, more at grazing angles
    c += pow(saturate(dot(r, car_light.sun.xyz)), 96.0) * float3(1.0, 0.96, 0.88) * 0.9;
    return saturate(c);
}

// The cars' shadows (an enhancement): a polygon that receives them (its
// position's z positive) looks its point up in the sun's view of the cars,
// in each cascade (the map's left half: the near cars, right: the far ones);
// 3 x 3 samples soften the edge. 1: lit.
float cascade_lit(float3 l, float4 map, float4 depth, float half_x) {
    if (map.z <= 0.0) return 1.0;
    const float2 uv = (l.xy - map.xy) * map.zw;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    const float d = saturate((l.z - depth.x) * depth.y) - 0.004;
    const float2 texel = float2(0.5, 1.0) * car_light.depth.w; // a half is square, the map twice as wide
    const float2 at = float2(half_x + uv.x * 0.5, 1.0 - uv.y);
    const float x0 = half_x + texel.x, x1 = half_x + 0.5 - texel.x; // the taps stay in their half
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 p = float2(clamp(at.x + float(x) * texel.x, x0, x1), at.y + float(y) * texel.y);
            lit += d <= car_shadow.SampleLevel(car_shadow_smp, p, 0.0) ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

float car_shadow_light(PolyOut i) {
    if (car_light.depth.z <= 0.0 || i.position_oz.z <= 0.0) return 1.0;
    const float3 p = i.position_oz / i.oz.x;
    const float3 l = float3(dot(p, car_light.right.xyz), dot(p, car_light.up.xyz), dot(p, car_light.dir.xyz));
    const float lit = cascade_lit(l, car_light.map, car_light.depth, 0.0) * cascade_lit(l, car_light.map2, car_light.depth2, 0.5);
    return lerp(1.0 - car_light.depth.z, 1.0, lit);
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
    const bool smooth = ((flags >> 29) & 1u) != 0u, reflective = ((flags >> 30) & 1u) != 0u;
    // a car's corner light, blended across the face (bit 29)
    const uint corner_light = uint(clamp(i.light_oz / i.oz.x + 0.5, 0.0, 255.0));
    if (renderer < 2u) { // solid: its palette colour, from the CPU
        if (!smooth && !reflective && i.position_oz.z <= 0.0) return i.color;
        float3 rgb = i.color.rgb;
        if (smooth) { // the CPU's colour (luma >> 2 through the translation), at this pixel's light
            const uint c = i.more.y, l = corner_light >> 2;
            rgb = float3(xlat[(((c >> 0) & 0x1fu) << 8) + l], xlat[0x2000u + (((c >> 5) & 0x1fu) << 8) + l],
                         xlat[0x4000u + (((c >> 10) & 0x1fu) << 8) + l]) / 255.0;
        }
        return float4((reflective ? car_paint(rgb, i) : rgb) * car_shadow_light(i), 1.0);
    }

    // raster.cpp draw_tex_span, one pixel
    const bool translucent = renderer == 3u;
    const int mml = -int(i.more.x) + fast_log2(z);
    const int u = to_s32(i.oz.y * z * 256.0);
    const int v = to_s32(i.oz.z * z * 256.0);
    uint t;
    const uint aniso = (flags >> 26) & 7u;
    if (aniso != 0u && !translucent) {
        // Anisotropic filtering (an enhancement): the pixel's footprint on the
        // texture, in texels; the level is made as many levels finer as the
        // samples taken along its long side cover (log2 n), from the game's own
        const float2 ax = ddx(uv), ay = ddy(uv);
        const float lx = length(ax), ly = length(ay);
        const float major = max(lx, ly), minor = max(min(lx, ly), 1e-6);
        const float2 along = lx > ly ? ax : ay;
        const uint n = uint(clamp(ceil(major / minor), 1.0, float(1u << aniso)));
        const int mml_n = mml - int(log2(float(n)) * 128.0);
        uint index = 0u, opacity = 0u;
        [loop] for (uint k = 0u; k < n; ++k) {
            const float2 o = along * ((float(k) + 0.5) / float(n) - 0.5);
            const uint s = sample_tex(i.info, mml_n, u + int(o.x * 256.0), v + int(o.y * 256.0), false,
                                      footprint / float(n));
            index += s & 0xffu;
            opacity += s >> 16;
        }
        t = (index / n) | ((opacity / n) << 16);
    } else {
        t = sample_tex(i.info, mml, u, v, translucent, footprint);
    }
    float alpha = 1.0;
    if (translucent) {
        if ((flags >> 25) & 1u) {
            // MSAA: the texels' opacity (0..0x80) covers as many samples
            // (alpha to coverage); half covers half, where the game cuts
            if (t < 0x00010000u) discard;
            alpha = saturate(float(t >> 16) / 128.0);
        } else if (t < 0x00400000u) discard;
        t &= 0xffu;
    }
    const uint lumabase = i.info.w & 0xffffu, pluma = smooth ? corner_light : i.info.w >> 16;
    uint luma = ((lumaram[lumabase + (t >> 1)] & 0xffu) * pluma / 256u) & 0xffu;
    luma = min(luma, 0x3fu);
    const uint c = i.more.y;
    const uint cr = ((c >> 0) & 0x1fu) << 8, cg = 0x2000u + (((c >> 5) & 0x1fu) << 8), cb = 0x4000u + (((c >> 10) & 0x1fu) << 8);
    const float3 rgb = float3(float(xlat[cr + luma]) / 255.0, float(xlat[cg + luma]) / 255.0, float(xlat[cb + luma]) / 255.0);
    return float4((reflective ? car_paint(rgb, i) : rgb) * car_shadow_light(i), alpha);
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
float4 tiles_back(float2 pixel) {
    const int margin = int(tiledata[0]);
    const uint fill = tiledata[1];
    const int out_w = kTileW + 2 * margin;
    const int2 xy = tile_position(pixel, margin);
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

float4 ps_tiles_back(QuadOut i) : SV_Target { return tiles_back(i.pos.xy); }

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

// The cars' shadow map: the cars seen from the sun, depth only. The CPU gives
// each corner already in the map's clip space (x, y) and depth (0..1).
struct ShadowIn {
    float2 pos : TEXCOORD0;
    float depth : TEXCOORD1;
};
float4 vs_shadow(ShadowIn i) : SV_Position { return float4(i.pos, i.depth, 1.0); }
void ps_shadow() {}

// The display passes (enhancements, GpuRenderer::present): the frame shown
// in the window through a bloom (the bright parts glow) and an arcade
// monitor's look (CRT: the game's 384 scanlines, an aperture grille, the
// corners darker). Levels are taken as gamma 2.2 for the light added.
// A full-screen triangle; uv 0..1 across the viewport.
struct PostOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
PostOut vs_post(uint vertex : SV_VertexID) {
    PostOut o;
    o.uv = float2((vertex << 1) & 2, vertex & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

struct Post {
    float2 src_uv;    // the frame's part of the source texture
    float src_lod;    // the source's mip level shown
    float lines;      // the game's lines (the scanlines)
    float2 step;      // the pass's input texel, in its uv (the blur: along its direction)
    float2 dst_size;  // the viewport, in pixels
    float bloom;      // the glow added (0: none)
    float crt;        // the monitor's look (0: none)
    float threshold;  // the light from which things glow
    float pad;
};
[[vk::binding(0, 3)]] cbuffer PostUBO : register(b0, space3) { Post post; };
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> post_src : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState post_src_smp : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] Texture2D<float4> post_glow : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState post_glow_smp : register(s1, space2);

float3 post_light(float3 level) { return pow(max(level, 0.0), 2.2); }

// The bright parts, at a quarter of the window: 4 x 4 samples per pixel (the
// source is up to 8 times finer), only the light above the threshold kept,
// with a soft knee.
float4 ps_post_bright(PostOut i) : SV_Target {
    float3 sum = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            const float2 uv = (i.uv + (float2(x, y) - 1.5) * 0.25 * post.step) * post.src_uv;
            sum += post_light(post_src.SampleLevel(post_src_smp, uv, post.src_lod).rgb);
        }
    const float3 c = sum / 16.0;
    const float peak = max(c.r, max(c.g, c.b));
    const float knee = 0.25;
    float soft = clamp(peak - post.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    return float4(c * (max(soft, peak - post.threshold) / max(peak, 1e-5)), 1.0);
}

// A 9-tap Gaussian along post.step, in 5 bilinear samples.
float4 ps_post_blur(PostOut i) : SV_Target {
    float3 c = post_src.SampleLevel(post_src_smp, i.uv, 0.0).rgb * 0.2270270270;
    c += post_src.SampleLevel(post_src_smp, i.uv + post.step * 1.3846153846, 0.0).rgb * 0.3162162162;
    c += post_src.SampleLevel(post_src_smp, i.uv - post.step * 1.3846153846, 0.0).rgb * 0.3162162162;
    c += post_src.SampleLevel(post_src_smp, i.uv + post.step * 3.2307692308, 0.0).rgb * 0.0702702703;
    c += post_src.SampleLevel(post_src_smp, i.uv - post.step * 3.2307692308, 0.0).rgb * 0.0702702703;
    return float4(c, 1.0);
}

// The frame into the window's viewport, with the glow and the monitor.
float4 ps_post(PostOut i) : SV_Target {
    float3 c = post_light(post_src.SampleLevel(post_src_smp, i.uv * post.src_uv, post.src_lod).rgb);
    if (post.bloom > 0.0) c += post_glow.SampleLevel(post_glow_smp, i.uv, 0.0).rgb * post.bloom;
    if (post.crt > 0.0) {
        // the scanlines: a beam per game line, wider where brighter, its
        // light kept on average; faded out where a line is under 3 pixels
        const float per_line = post.dst_size.y / post.lines;
        const float lines_on = post.crt * saturate((per_line - 1.5) / 1.5);
        const float d = frac(i.uv.y * post.lines) - 0.5;
        const float luma = dot(min(c, 1.0), float3(0.299, 0.587, 0.114));
        const float sigma = lerp(0.20, 0.36, luma);
        const float beam = exp(-0.5 * d * d / (sigma * sigma)) / min(sigma * 2.5066, 1.0);
        c *= lerp(1.0, beam, lines_on);
        // the aperture grille: red, green and blue columns, from 4 pixels a line
        const float mask_on = post.crt * saturate((per_line - 2.5) / 1.5) * 0.3;
        const uint column = uint(i.pos.x) % 3u;
        const float3 mask = column == 0u ? float3(1.0, 0.0, 0.0) : column == 1u ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
        c *= 1.0 - mask_on + 3.0 * mask_on * mask; // the same light on average
        // the corners a little darker
        const float2 e = i.uv * (1.0 - i.uv);
        c *= lerp(1.0, pow(saturate(e.x * e.y * 16.0), 0.12), post.crt);
    }
    return float4(pow(saturate(c), 1.0 / 2.2), 1.0);
}

// The sky the cars' paint reflects (GpuRenderer::render, with the
// reflections): the back layers drawn whole into a small texture (uv across
// the target's viewport); its mip levels blur it.
float4 ps_sky_env(PostOut i) : SV_Target { return tiles_back(i.uv * float2(tiledata[3], tiledata[4])); }
