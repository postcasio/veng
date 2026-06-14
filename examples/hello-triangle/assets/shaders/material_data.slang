// Shared bindless material declarations for the forward material shader.
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

// One shared push-constant block, used by both stages: the vertex stage reads
// MVP, the fragment stage reads MaterialIndex (the per-draw material selector
// Material::Bind pushes at offset 64 = sizeof(float4x4)). Total 68 bytes, well
// under the 128-byte guaranteed minimum.
struct PushConstants
{
    float4x4 MVP;
    uint MaterialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;
