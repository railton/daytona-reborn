cbuffer Camera : register(b0, space1) {
    float4 eye;
    float4 right;    // w: horizontal projection scale
    float4 up;       // w: vertical projection scale
    float4 forward;  // w: horizontal lens shift
    float4 focus;    // tilted focus plane in world space
};
cbuffer Object : register(b1, space1) {
    float4 position; // w: cos(yaw)
    float4 paint;    // w: sin(yaw)
};

struct Surface {
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float focusDistance : TEXCOORD2;
};

float3 rotate_y(float3 p) {
    return float3(position.w * p.x + paint.w * p.z, p.y, -paint.w * p.x + position.w * p.z);
}

Surface vs_home(float3 p : TEXCOORD0, float3 n : TEXCOORD1, float4 color : TEXCOORD2) {
    Surface o;
    float3 world = rotate_y(p) + position.xyz;
    float3 v = world - eye.xyz;
    float z = dot(forward.xyz, v);
    o.position = float4(dot(right.xyz, v) * right.w + z * forward.w,
                       dot(up.xyz, v) * up.w, 1.002 * z - 0.501, z);
    o.color = lerp(color.rgb, paint.rgb, color.a);
    o.normal = rotate_y(n);
    o.focusDistance = dot(world, focus.xyz) - focus.w;
    return o;
}

float4 ps_home(Surface i) : SV_Target {
    const float3 sun = float3(-0.51, 0.82, -0.26);
    float light = saturate(dot(normalize(i.normal), sun));
    float3 color = i.color * lerp(float3(0.59, 0.69, 0.81), float3(1.12, 1.06, 0.94), light);
    float coc = saturate((abs(i.focusDistance) - 2.0) / 20.0);
    return float4(color, coc);
}

struct Quad {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Quad vs_lens(uint vertex : SV_VertexID) {
    Quad o;
    o.uv = float2((vertex << 1) & 2, vertex & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

#ifdef __spirv__
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
#endif
Texture2D<float4> scene : register(t0, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
#endif
SamplerState lens : register(s0, space2);
cbuffer Lens : register(b0, space3) {
    float4 lensParams; // aspect ratio, transition opacity
};

float4 ps_lens(Quad i) : SV_Target {
    float4 center = scene.Sample(lens, i.uv);
    float2 r = center.a * 0.008 * float2(1 / lensParams.x, 1);
    float3 color = center.rgb * 2;
    float total = 2;
    // A compact aperture with a CoC rejection term to limit foreground bleed.
    const float2 disk[12] = {
        float2(1, 0), float2(0.71, 0.71), float2(0, 1), float2(-0.71, 0.71),
        float2(-1, 0), float2(-0.71, -0.71), float2(0, -1), float2(0.71, -0.71),
        float2(0.18, 0.41), float2(-0.41, 0.18), float2(-0.18, -0.41), float2(0.41, -0.18)
    };
    if (center.a > 0.01) {
        for (uint j = 0; j < 12; ++j) {
            float4 sampleColor = scene.Sample(lens, i.uv + disk[j] * r);
            float weight = 1 - 0.8 * saturate((center.a - sampleColor.a) * 3);
            color += sampleColor.rgb * weight;
            total += weight;
        }
    }
    float left = 1 - smoothstep(0.02, 0.62, i.uv.x);
    float edge = saturate(dot(i.uv - 0.5, i.uv - 0.5));
    return float4(lerp(color / total, float3(0.025, 0.067, 0.106), left * 0.94)
                  * (1 - 0.30 * edge) * lensParams.y, 1);
}
