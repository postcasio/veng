// Scene-capture octahedral distance resample: the depth sibling of capture_octahedral.frag. It turns
// the six captured face depths (tiled in a 3×2 atlas, the same layout the colour atlas uses) into one
// octahedral map holding, per direction, the RADIAL distance from the probe centre to the nearest
// surface in that direction — in world units. Each output texel decodes its UV to a world direction,
// selects the face whose axis dominates it, projects onto that face's camera basis exactly as the
// colour resample does, samples the face's atlas depth there, linearizes it to a view-space forward
// distance, then divides by the direction's cosine with the face axis to recover the radial length.
//
// Radial, not view-space z. The map is indexed by direction, so the value a consumer needs at a
// direction is the distance ALONG that direction. The linearized depth is the distance along the
// FACE axis; dividing by dot(dir, faceForward) — the cosine already computed to project the face UV —
// turns it into the radial distance. Storing the face-axis distance instead would be wrong by 1/cosθ
// at every off-axis texel, up to a factor of √3 at a face corner, a correction the consumer cannot
// make because the octahedral map hides which face a texel came from.
//
// The face/basis table and the projection mirror capture_octahedral.frag.slang exactly — the two
// must not drift, or the distance map falls out of register with the radiance map it stands beside.
#include "Veng/octahedral.slang"
#include "Veng/capture_distance.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint DepthAtlasTexture; // the 3×2 face depth atlas (bindless index)
    uint Sampler;           // point clamp sampler (no filtering across a depth discontinuity)
    float Near;             // the face cameras' near plane, in world units
    float Far;              // the face cameras' far plane, in world units
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
float fsMain(VSOutput input) : SV_Target0
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

    // The face camera's look-at basis and its Vulkan Y-flipped 90°/aspect-1 projection, inverted into
    // a face UV. `axisCos` is dot(dir, faceForward), the cosine between the direction and the face
    // axis — the projection's own denominator, reused below to turn a face-axis distance into a radial
    // one.
    const float3 f = FaceForward[face];
    const float3 s = normalize(cross(f, FaceUp[face]));
    const float3 u = cross(s, f);
    const float axisCos = dot(dir, f);
    const float2 faceUv = float2(dot(dir, s) / axisCos, -dot(dir, u) / axisCos) * 0.5 + 0.5;

    // The 3×2 atlas cell (row-major: faces 0..2 top row, 3..5 bottom). A point sampler and a
    // half-texel inset keep the read on one face's own cell.
    Texture2D atlas = g_Textures[NonUniformResourceIndex(g_PC.DepthAtlasTexture)];
    uint atlasWidth;
    uint atlasHeight;
    atlas.GetDimensions(atlasWidth, atlasHeight);
    const float2 halfTexel = 0.5 / float2(atlasWidth, atlasHeight);
    const float2 cell = float2(face % 3, face / 3);
    const float2 cellScale = float2(1.0 / 3.0, 0.5);
    const float2 atlasUv = (cell + clamp(faceUv, 0.0, 1.0)) * cellScale;
    const float2 clamped = clamp(atlasUv, cell * cellScale + halfTexel,
                                 (cell + 1.0) * cellScale - halfTexel);

    const float depth =
        atlas.SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], clamped, 0.0).r;

    // Reverse-Z: the depth buffer clears to 0 at the far plane and a fragment writes 1 at the near
    // plane, so a texel that recorded no geometry (only the cleared far plane, which is the sky) reads
    // 0. That direction has no surface — it stores the sky sentinel.
    if (depth <= 0.0)
    {
        return CaptureDistanceSky;
    }

    // Linearize the reverse-Z depth to the view-space forward distance (along the face axis). With the
    // engine's reverse-Z convention (near→1, far→0) over a Vulkan [0,1] projection, the forward
    // distance is near·far / (far − (1 − depth)·(far − near)). Dividing by the axis cosine recovers the
    // radial distance along `dir`, which is what a direction-indexed map must carry.
    const float forward =
        (g_PC.Near * g_PC.Far) / (g_PC.Far - (1.0 - depth) * (g_PC.Far - g_PC.Near));
    return forward / max(axisCos, 1.0e-4);
}
