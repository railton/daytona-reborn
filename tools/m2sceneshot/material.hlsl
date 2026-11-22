#define vs_scene project_scene
#define ps_scene shade_diagnostic
#include "scene.hlsl"
#undef vs_scene
#undef ps_scene

#include "../../src/app/gpu/m2.hlsl"

cbuffer MaterialDraw : register(b1, space1) {
    uint4 material_info;
    uint4 material_more;
};

struct MaterialIn {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct MaterialVertex {
    float4 position : SV_Position;
    float3 uv_depth : TEXCOORD0;
    nointerpolation float4 colour : TEXCOORD1;
    nointerpolation uint4 info : TEXCOORD2;
    nointerpolation uint2 more : TEXCOORD3;
    float2 view_clip : SV_ClipDistance0;
};

MaterialVertex vs_scene(MaterialIn vertex) {
    SceneVertex source;
    source.position = vertex.position;
    const ProjectedVertex projected = project_scene(source);
    MaterialVertex result;
    result.position = projected.position;
    if (material_more.z != 0)
        result.position.z = asfloat(material_more.z) * projected.position.w;
    result.view_clip = material_more.w != 0
        ? float2(projected.position.w - viewport.z, viewport.w - projected.position.w)
        : float2(1, 1);
    result.uv_depth = float3(vertex.uv * 0.125, projected.position.w);
    result.colour = projected.colour;
    result.info = material_info;
    result.more = material_more.xy;
    return result;
}

float4 ps_scene(MaterialVertex input) : SV_Target0 {
    // Smooth UV and view depth are perspective-correct. Convert them to
    // the reciprocal-depth quantities used by the original fragment kernel.
    const float inv_z = rcp(input.uv_depth.z);
    PolyOut state;
    state.pos = input.position;
    state.oz = float3(1, input.uv_depth.xy) * inv_z;
    state.color = input.colour;
    state.info = input.info;
    state.more = input.more;
    // The legacy SDL GPU kernel evaluates checker coverage at the target
    // pixel grid, which is doubled in this viewer.
    return ps_poly(state);
}
