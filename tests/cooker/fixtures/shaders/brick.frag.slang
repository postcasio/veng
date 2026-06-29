// Deferred opaque material fragment stage (cooker fixture): loads the per-draw
// MaterialParams block, samples its albedo texture/sampler by handle tinted by
// Factors, and writes the three-target g-buffer — albedo into G0, the world-space
// normal into G1, and a constant packed ORM into G2 (the surface output contract).
//
// This fixture is deliberately self-contained — its MaterialParams (a single Factors
// tint) and its MVP/MaterialIndex push block exercise the cook machinery generically,
// not the engine's PBR material contract. It is the sibling of handles_only.frag /
// params_only.frag / oversize.frag, which are self-contained the same way.

// MaterialParams is this fixture's whole parameter block — the bindless handle slots
// (uint members the loader patches with a resolved index) beside the authored Factors
// tint, laid out by reflection. The cooker reflects this struct to validate a material's
// fields and pack their values at the reflected offsets, read at index * MaterialParamStride.
struct MaterialParams
{
    uint   Albedo;        // bindless sampled-image index (texture handle)
    uint   AlbedoSampler; // bindless sampler index (sampler handle)
    uint   Pad0;
    uint   Pad1;
    float4 Factors;       // authored param
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// The deferred opaque g-buffer outputs the material fragment stage writes: albedo
// into G0 (sRGB), the world-space normal into G1, packed
// occlusion/roughness/metallic/emissive into G2, and the motion vector into G3 — the
// surface output contract.
struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 ORM      : SV_Target2;
    float2 Velocity : SV_Target3;  // screen-space motion vector (curUV - prevUV)
};

// One shared push-constant block, used by both stages. The vertex stage reads MVP
// (offset 0) and NormalMatrix (offset 80); the fragment stage reads MaterialIndex (the
// per-draw selector at offset 64). The block is exactly 128 bytes — the guaranteed-
// minimum push-constant size. Matrices are column-major (the cooker compiles Slang
// column-major), matching the glm matrices veng pushes verbatim.
struct PushConstants
{
    float4x4 MVP;          // offset 0
    uint MaterialIndex;    // offset 64
    uint Pad0;             // offset 68 — three scalar pads carry the cursor to
    uint Pad1;             // offset 72   NormalMatrix's 16-byte boundary at 80
    uint Pad2;             // offset 76
    float3x3 NormalMatrix; // offset 80 (48 bytes) — ends at 128
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 albedo = g_Textures[NonUniformResourceIndex(p.Albedo)].Sample(
        g_Samplers[NonUniformResourceIndex(p.AlbedoSampler)], input.v_UV);

    GBufferOutput output;
    output.Albedo = albedo * p.Factors;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = float2(0.0, 0.0);
    return output;
}
