// The engine's material contract: the bindless declarations, per-frame view block,
// per-draw data, g-buffer output, motion-vector helper, the domain-keyed push blocks,
// and the per-domain fragment-input struct a Surface or PostProcess fragment reads.
// A consumer (or generated) shader `#include`s this and declares its own MaterialParams
// beside it — the parameter block is per-shader by definition (the cooker reflects each
// shader's own struct to pack its fields at the reflected offsets), so it is not part of
// the engine contract.
//
// The set-0 bindings mirror the BindlessRegistry layout (sampled images = 0, samplers = 1,
// per-material block = 4, view constants = 5).

// The per-material block byte stride. Mirrors BindlessRegistry::MaterialParamStride; one
// stride is shared across every material so a single ByteAddressBuffer holds a different
// per-shader parameter layout, read at index * MaterialParamStride.
static const uint MaterialParamStride = 256;

// The canonical per-frame view-constants block (set 0, binding 5) is declared in
// view_constants.slang — the single source for the lighting, SSAO, and material shaders.
#include "../view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

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
    uint     PaletteBase;    // skinned meshes: base offset of this instance's bones in g_Palette
    uint     PrevPaletteBase; // skinned meshes: previous frame's palette base (TAA velocity)
    uint     EntityIndex;    // packed Entity slot index; the picking fragment writes index + 1
    float4x4 PrevWorld;      // previous frame's model matrix (TAA velocity; equals World with no motion)
    float4   EmissiveColor;  // rgb additive emissive term (a unused); the emissive forward pass reads it
    uint     EmissiveTexture; // emissive texture bindless handle (0 = none, flat color)
    uint     EmissiveSampler; // emissive sampler bindless handle
    uint     EmissivePad0;
    uint     EmissivePad1;
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

// The deferred opaque g-buffer outputs the material fragment stage writes. The
// metallic-roughness channel set plus the screen-space motion vector, funnelled
// through one struct: albedo into G0 (sRGB target, the sampler decodes to linear on
// read), the world-space normal into G1, packed occlusion/roughness/metallic
// into G2, and the per-object motion vector into G3 (the TAA resolve reprojects through
// it). G3 is written every frame by the same surface pipeline, so motion vectors cost
// no second geometry pass; with TAA off the channel is written but unread. Emissive is
// not a g-buffer channel — it is an additive forward pass into the lit HDR target.
struct GBufferOutput
{
    float4 Albedo   : SV_Target0;  // rgb sRGB albedo; a unused
    float4 Normal   : SV_Target1;  // world-space normal in xyz
    float4 ORM      : SV_Target2;  // R occlusion, G roughness, B metallic, A unused
    float2 Velocity : SV_Target3;  // screen-space motion vector (curUV - prevUV)
};

// The per-object screen-space motion vector from the vertex stage's unjittered current
// and previous clip positions: the UV displacement from this pixel's previous position
// to its current one, so the TAA resolve fetches history at uv - velocity. Camera and
// object motion combine automatically — a static object under a moving camera reprojects
// with the camera (its previous world equals its current), a moving object adds its own
// displacement on top. The clip positions are unjittered (CurViewProj / PrevViewProj),
// so the jitter TAA is integrating does not leak into the reprojection.
float2 ComputeMotionVector(float4 curClip, float4 prevClip)
{
    const float2 curUv = (curClip.xy / curClip.w) * 0.5 + 0.5;
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    return curUv - prevUv;
}

// The surface push block, used by the vertex stage and read by a Surface fragment's
// LoadDrawData. FrameBase folds the ring-buffered draw-data region into the candidate id
// (the current frame's region base); ViewConstantsIndex selects the per-frame set-0
// view-constants region the vertex stage multiplies by. A Surface fragment reads its
// material selector from the v_MaterialIndex interpolant, not from this block. Matrices
// are column-major (the cooker compiles Slang column-major), matching the glm matrices
// veng uploads verbatim.
struct PushConstants
{
    uint FrameBase;          // current frame's g_DrawData region base
    uint ViewConstantsIndex; // current frame's view-constants region
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The interpolants a Surface fragment reads, written by the standard surface vertex
// stage (surface.vert). A surface fragment's fsMain signature takes this struct.
struct SurfaceFragmentInput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    float4 v_WorldTangent : TANGENT;  // xyz world tangent; w handedness sign for the bitangent
    nointerpolation uint v_MaterialIndex : TEXCOORD1;  // per-draw material selector from the vertex stage
    float4 v_CurClip : TEXCOORD2;   // unjittered current clip position (g-buffer motion vector)
    float4 v_PrevClip : TEXCOORD3;  // unjittered previous clip position (g-buffer motion vector)
};

// The screen-UV interpolant a PostProcess fragment reads, written by the fullscreen
// vertex stage (fullscreen.vert). A PostProcess fragment's fsMain signature takes this
// struct and reads its material selector from the PostProcessPushConstants block, since
// it has no per-draw v_MaterialIndex interpolant.
struct PostProcessFragmentInput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// A PostProcess fragment's per-draw selector sits at push-constant offset 0 (no geometry
// block precedes it, unlike a Surface shader's MVP). A PostProcess fragment reads its
// material index here rather than from a vertex interpolant.
struct PostProcessPushConstants
{
    uint MaterialIndex;
};
