// Shared bindless material declarations for the deferred opaque material shader.
// MaterialParams is the material's whole parameter block — the bindless handle
// slots (uint members the loader patches with a resolved index) beside the
// author's scalar/vector uniforms (here, a single Factors tint), laid out by
// reflection. The set-0 bindings mirror the BindlessRegistry layout (sampled
// images = 0, samplers = 1, per-material block = 4). The cooker reflects this
// struct to validate a material's fields and pack their values at the reflected
// offsets.

struct MaterialParams
{
    uint   Albedo;        // bindless sampled-image index (texture handle)
    uint   AlbedoSampler; // bindless sampler index (sampler handle)
    uint   Pad0;
    uint   Pad1;
    float4 Factors;       // authored param
};

// The per-material block byte stride. Mirrors
// BindlessRegistry::MaterialParamStride; one stride is shared across every
// material so a single ByteAddressBuffer holds a different MaterialParams layout
// per shader, read at index * MaterialParamStride.
static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// The deferred opaque g-buffer outputs the material fragment stage writes. The
// v1 minimum channel set, funnelled through one struct so a future G2
// (roughness/metallic/AO) extends it in one place. Albedo's target is sRGB
// (the sampler decodes to linear on read); Normal is the world-space normal.
struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

// One shared push-constant block, used by both stages. The vertex stage reads
// MVP (offset 0) and NormalMatrix (offset 80, the inverse-transpose of the
// model's upper 3x3 that carries the normal to world space); the fragment stage
// reads MaterialIndex (the per-draw selector Material::Bind pushes at offset 64
// = sizeof(float4x4)). MaterialIndex sits between MVP and NormalMatrix, padded
// to NormalMatrix's 16-byte alignment. The block is exactly 128 bytes — the
// guaranteed-minimum push-constant size. Matrices are column-major (the cooker
// compiles Slang column-major), matching the glm matrices veng pushes verbatim.
struct PushConstants
{
    float4x4 MVP;          // offset 0
    uint MaterialIndex;    // offset 64
    uint Pad0;             // offset 68 — three scalar pads (4-byte aligned, no
    uint Pad1;             // offset 72   vector alignment bump) carry the cursor
    uint Pad2;             // offset 76   to NormalMatrix's 16-byte boundary at 80
    float3x3 NormalMatrix; // offset 80 (48 bytes) — ends at 128
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;
