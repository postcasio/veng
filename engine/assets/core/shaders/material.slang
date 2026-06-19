// Byte-identical to examples/hello-triangle/assets/shaders/material_data.slang;
// the two must stay in sync until the example's fragment include resolves this
// core header directly (a cross-pack Slang include path). The engine-side
// MaterialParams layout (Veng/Renderer/MaterialParams.h) is static_assert-guarded,
// so a divergence in the struct sizes/offsets is a build error, not a silent
// offset-patch misread.
//
// Shared bindless material declarations for the deferred opaque material shader.
// MaterialParams is the material's whole metallic-roughness parameter block — the
// bindless handle slots (uint members the loader patches with a resolved index)
// beside the authored scalar/vector uniforms, laid out by reflection. The set-0
// bindings mirror the BindlessRegistry layout (sampled images = 0, samplers = 1,
// per-material block = 4, view constants = 5). The cooker reflects this struct to
// validate a material's fields and pack their values at the reflected offsets.

struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;   // base-color texture handle + sampler
    uint   ORM;              uint ORMSampler;         // occlusion-roughness-metallic texture handle + sampler
    uint   Normal;           uint NormalSampler;      // tangent-space normal-map handle + sampler (0 = unbound)
    uint   Pad0;             uint Pad1;
    float4 BaseColorFactor;  // tint applied to the sampled base color
    float4 EmissiveFactor;   // rgb reserved (colored emissive is a follow-on); a = strength
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float Pad2;
};

// The per-material block byte stride. Mirrors
// BindlessRegistry::MaterialParamStride; one stride is shared across every
// material so a single ByteAddressBuffer holds a different MaterialParams layout
// per shader, read at index * MaterialParamStride.
static const uint MaterialParamStride = 256;

// The per-frame view-constants block: per-view data the deferred lighting pass
// reads (world-position reconstruction + the camera position). Ring-buffered
// across frames-in-flight at binding 5, byte-addressed at
// index * ViewConstantsStride; mirrors BindlessRegistry::ViewConstantsStride.
// Lights ride a separate ring-buffered light buffer at binding 6.
struct ViewConstants
{
    float4x4 InvViewProj;       // world-position reconstruction from depth
    float4   CameraPosition;    // xyz; w unused
    float4x4 View;              // world → view (SSAO reconstructs view space)
    float4x4 Proj;              // view → clip
};

static const uint ViewConstantsStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;
[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

ViewConstants LoadViewConstants(uint index)
{
    return g_ViewConstants.Load<ViewConstants>(index * ViewConstantsStride);
}

// The deferred opaque g-buffer outputs the material fragment stage writes. The
// metallic-roughness channel set, funnelled through one struct: albedo into G0
// (sRGB target, the sampler decodes to linear on read), the world-space normal
// into G1, and packed occlusion/roughness/metallic/emissive into G2.
struct GBufferOutput
{
    float4 Albedo : SV_Target0;  // rgb sRGB albedo; a unused
    float4 Normal : SV_Target1;  // world-space normal in xyz
    float4 ORM    : SV_Target2;  // R occlusion, G roughness, B metallic, A emissive strength
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
