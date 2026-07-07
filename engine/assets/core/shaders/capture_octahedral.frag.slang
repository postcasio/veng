// Scene-capture octahedral resample fragment stage: turns the six captured face renders
// (tiled in a 3×2 atlas) into one octahedral 2D map. Each output pixel decodes its UV to a
// world direction, selects the face whose axis dominates it, projects onto that face's
// camera basis (90° fov, aspect 1 — the same look-at projection the face render used, Vulkan
// Y-flip included), and samples the face's atlas cell. The face/basis table mirrors
// SceneCapture.cpp's FaceForward/FaceUp exactly — the two must not drift.
#include "Veng/octahedral.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint AtlasTexture; // the 3×2 face atlas (bindless index)
    uint Sampler;      // shared clamp sampler
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Face order and bases: +X, -X, +Y, -Y, +Z, -Z (mirrors SceneCapture.cpp).
static const float3 FaceForward[6] = {
    float3(1.0, 0.0, 0.0),  float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0),
    float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0),  float3(0.0, 0.0, -1.0),
};
static const float3 FaceUp[6] = {
    float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0),  float3(0.0, 0.0, -1.0),
    float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 0.0),  float3(0.0, 1.0, 0.0),
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    const float3 dir = OctahedralDir(input.v_UV);

    // The face whose axis dominates the direction owns it.
    const float3 a = abs(dir);
    int face;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = dir.x > 0.0 ? 0 : 1;
    }
    else if (a.y >= a.z)
    {
        face = dir.y > 0.0 ? 2 : 3;
    }
    else
    {
        face = dir.z > 0.0 ? 4 : 5;
    }

    // The face camera's look-at basis (glm: side = cross(forward, up), up' = cross(side,
    // forward)) and the Vulkan Y-flipped 90°/aspect-1 projection, inverted into a face UV.
    const float3 f = FaceForward[face];
    const float3 s = normalize(cross(f, FaceUp[face]));
    const float3 u = cross(s, f);
    const float depth = dot(dir, f);
    const float2 faceUv =
        float2(dot(dir, s) / depth, -dot(dir, u) / depth) * 0.5 + 0.5;

    // The 3×2 atlas cell (row-major: faces 0..2 in the top row, 3..5 in the bottom). A
    // half-texel inset keeps a bilinear tap at a face edge from bleeding into the neighbor.
    Texture2D atlas = g_Textures[NonUniformResourceIndex(g_PC.AtlasTexture)];
    uint atlasWidth;
    uint atlasHeight;
    atlas.GetDimensions(atlasWidth, atlasHeight);
    const float2 halfTexel = 0.5 / float2(atlasWidth, atlasHeight);
    const float2 cell = float2(face % 3, face / 3);
    const float2 cellScale = float2(1.0 / 3.0, 0.5);
    const float2 atlasUv = (cell + clamp(faceUv, 0.0, 1.0)) * cellScale;
    const float2 clamped = clamp(atlasUv, cell * cellScale + halfTexel,
                                 (cell + 1.0) * cellScale - halfTexel);

    const float3 color =
        atlas.SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], clamped, 0.0).rgb;
    return float4(color, 1.0);
}
