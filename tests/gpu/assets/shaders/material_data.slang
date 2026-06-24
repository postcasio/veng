// Mirrors engine/assets/core/shaders/material.slang; the two stay in sync until the
// example's fragment include resolves the core header directly (a cross-pack Slang
// include path). The view-constants block the core copy pulls from view_constants.slang
// is inlined here, since that file lives in the core pack's shader dir, not the
// example's Slang search path. The engine-side MaterialParams layout
// (Veng/Renderer/MaterialParams.h) is static_assert-guarded, so a divergence in the
// struct sizes/offsets is a build error.
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

// The canonical per-frame view-constants block (set 0, binding 5), inlined from the
// core view_constants.slang (the example's Slang search path does not reach the core
// pack's shader dir). Ring-buffered across frames-in-flight, byte-addressed at
// index * ViewConstantsStride.
struct ViewConstants
{
    float4x4 InvViewProj;    // world-position reconstruction from depth
    float4   CameraPosition; // xyz; w unused
    float4x4 View;           // world → view
    float4x4 Proj;           // view → clip
    float4x4 PrevViewProj;   // previous frame's unjittered world → clip (g-buffer motion vector)
    float4x4 CurViewProj;    // this frame's unjittered world → clip (g-buffer motion vector)
};

static const uint ViewConstantsStride = 512;

[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;

ViewConstants LoadViewConstants(uint index)
{
    return g_ViewConstants.Load<ViewConstants>(index * ViewConstantsStride);
}

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// Per-draw record indexed by the candidate id, std430-identical to the engine's
// DrawData (Veng/Renderer/SceneRenderer's GpuDrawData). The geometry pass uploads
// one record per per-submesh candidate into a ring-buffered SSBO; the vertex stage
// reads its record by the candidate id carried in the instance-rate attribute,
// replacing the per-draw MVP / NormalMatrix / MaterialIndex push constants. Both
// CullMode::CPU (a direct DrawIndexed per submesh) and CullMode::GPU (one
// DrawIndexedIndirect per mesh group) drive the same buffer, differing only in
// submission and in who writes the indirect command's instanceCount.
struct DrawData
{
    float4x4 World;          // per-instance model matrix (replaces the MVP push)
    float4   NormalColumn0;  // inverse-transpose of World's upper 3x3, column 0 (xyz; w pad)
    float4   NormalColumn1;  // column 1
    float4   NormalColumn2;  // column 2
    uint     MaterialIndex;  // GetCurrentFrameBase()-folded material selector
    uint     Pad0;
    uint     Pad1;
    uint     Pad2;
    float4x4 PrevWorld;      // previous frame's model matrix (TAA velocity; equals World with no motion)
};

// The per-draw data SSBO (set 1, binding 0). Off the set-0 bindless registry — a
// closed producer→consumer per-frame buffer needs no global registration, and a
// dedicated set sidesteps the set-0 storage-buffer argument-buffer path on MoltenVK.
// Ring-buffered for frames-in-flight; the current frame's region base is folded into
// the candidate id by the pushed FrameBase, exactly as the material block selector is.
[[vk::binding(0, 1)]] StructuredBuffer<DrawData> g_DrawData;

DrawData LoadDrawData(uint candidateId)
{
    return g_DrawData[g_PC.FrameBase + candidateId];
}

// The deferred opaque g-buffer outputs the material fragment stage writes: albedo into
// G0, the world-space normal into G1, packed occlusion/roughness/metallic/emissive into
// G2, and the per-object screen-space motion vector into G3 (written every frame by the
// same surface pipeline, so motion vectors cost no second geometry pass).
struct GBufferOutput
{
    float4 Albedo   : SV_Target0;  // rgb sRGB albedo; a unused
    float4 Normal   : SV_Target1;  // world-space normal in xyz
    float4 ORM      : SV_Target2;  // R occlusion, G roughness, B metallic, A emissive strength
    float2 Velocity : SV_Target3;  // screen-space motion vector (curUV - prevUV)
};

// The per-object screen-space motion vector from the vertex stage's unjittered current
// and previous clip positions (the UV displacement from previous to current position).
float2 ComputeMotionVector(float4 curClip, float4 prevClip)
{
    const float2 curUv = (curClip.xy / curClip.w) * 0.5 + 0.5;
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    return curUv - prevUv;
}

// The shared surface push block, used by the vertex stage only. FrameBase folds the
// ring-buffered draw-data region into the candidate id (the current frame's region
// base); ViewConstantsIndex selects the per-frame set-0 view-constants region the
// vertex stage multiplies by. Both replace the former per-draw MVP / NormalMatrix /
// MaterialIndex pushes — the per-draw values now live in g_DrawData, indexed by the
// candidate id. Matrices are column-major (the cooker compiles Slang column-major),
// matching the glm matrices veng uploads verbatim.
struct PushConstants
{
    uint FrameBase;          // current frame's g_DrawData region base
    uint ViewConstantsIndex; // current frame's view-constants region
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;
