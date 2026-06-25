// Equirectangular HDR panorama -> radiance cubemap. One dispatch fills all six faces of
// one mip: id.xy is the face texel, id.z is the face index. Each invocation maps its face
// texel to a world direction, projects that direction into the panorama's equirectangular
// UV, and writes the sampled radiance into the cube layer.
//
// The panorama is sampled through the set-0 bindless array (it is a registered asset);
// the destination is a single storage layer set bound on set 1.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2DArray<float4> g_DestCube;

struct PushConstants
{
    uint Equirect; // bindless index of the panorama texture
    uint Sampler;  // bindless index of the panorama sampler
    uint FaceSize; // destination face edge length in texels
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const float Pi = 3.14159265358979323846;

// World direction for a face texel. uv is the texel center in [0,1]; remap to [-1,1] and
// build the standard cube-face basis (matching the Cube image-view layer order).
float3 FaceDirection(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
    case 0: dir = float3(1.0, -st.y, -st.x); break;  // +X
    case 1: dir = float3(-1.0, -st.y, st.x); break;  // -X
    case 2: dir = float3(st.x, 1.0, st.y); break;    // +Y
    case 3: dir = float3(st.x, -1.0, -st.y); break;  // -Y
    case 4: dir = float3(st.x, -st.y, 1.0); break;   // +Z
    default: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    }
    return normalize(dir);
}

// Equirectangular UV for a direction: longitude from atan2, latitude from asin. V is
// flipped so the panorama's top row (stored first) maps to +Y.
float2 EquirectUV(float3 dir)
{
    float2 uv = float2(atan2(dir.z, dir.x), asin(clamp(dir.y, -1.0, 1.0)));
    uv *= float2(0.5 / Pi, 1.0 / Pi);
    uv += 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.FaceSize || id.y >= g_PC.FaceSize)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float(g_PC.FaceSize);
    float3 dir = FaceDirection(id.z, uv);
    float2 euv = EquirectUV(dir);

    float3 color = g_Textures[NonUniformResourceIndex(g_PC.Equirect)]
                       .SampleLevel(g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], euv, 0.0)
                       .rgb;
    g_DestCube[uint3(id.xy, id.z)] = float4(color, 1.0);
}
