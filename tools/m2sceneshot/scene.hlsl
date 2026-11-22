cbuffer SceneDraw : register(b0, space1) {
    float4 view_row_x;
    float4 view_row_y;
    float4 view_row_z;
    // Focal lengths and centre in reference pixels, with a top-left origin.
    float4 projection;
    // Reference width, height, near and far distances.
    float4 viewport;
    float4 material_colour;
};

struct SceneVertex {
    float3 position : TEXCOORD0;
};

struct ProjectedVertex {
    float4 position : SV_Position;
    float4 colour : TEXCOORD0;
};

ProjectedVertex vs_scene(SceneVertex vertex) {
    float4 p = float4(vertex.position, 1);
    float3 view = float3(dot(view_row_x, p), dot(view_row_y, p), dot(view_row_z, p));
    float x = 2 * (projection.x * view.x + projection.z * view.z) / viewport.x - view.z;
    float y = view.z - 2 * (projection.w * view.z - projection.y * view.y) / viewport.y;
    // Reverse depth keeps precision for distant road surfaces with D32_FLOAT.
    float z = viewport.z * (viewport.w - view.z) / (viewport.w - viewport.z);
    ProjectedVertex result;
    result.position = float4(x, y, z, view.z);
    result.colour = material_colour;
    return result;
}

float4 ps_scene(ProjectedVertex input) : SV_Target0 {
    return input.colour;
}
