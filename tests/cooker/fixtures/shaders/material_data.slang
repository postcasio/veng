// Shared bindless material declarations for the deferred opaque material shader.
// MaterialData mirrors the engine's Renderer::MaterialData
// (BindlessRegistry.h) byte-for-byte; the set-0 bindings mirror the
// BindlessRegistry layout (sampled images = 0, samplers = 1, MaterialData
// array = 3). The cooker reflects MaterialData here to validate a material's
// textures/params and pack their values at the reflected offsets.

struct MaterialData
{
    uint Albedo;        // bindless sampled-image index
    uint AlbedoSampler; // bindless sampler index
    uint Pad0;
    uint Pad1;
    float4 Factors;
};

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] StructuredBuffer<MaterialData> g_Materials;

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
