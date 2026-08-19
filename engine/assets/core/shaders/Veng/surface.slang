// The engine's Surface material contract: the set-0 bindless declarations, the per-frame
// view block, the per-draw data, the g-buffer output, the motion-vector and normal helpers,
// the surface push block, and the fragment-input struct a Surface fragment reads. A Surface
// material (canonical-layout vertex stage, g-buffer MRT output, drawn per-submesh) `#include`s
// this and declares its own MaterialParams beside it — the parameter block is per-shader by
// definition (the cooker reflects each shader's own struct to pack its fields at the reflected
// offsets), so it is not part of the engine contract. The PostProcess and Sky domains have
// their own contracts in Veng/postprocess.slang and Veng/sky.slang.
//
// The set-0 bindings mirror the BindlessRegistry layout (sampled images = 0, samplers = 1,
// per-material block = 4, view constants = 5).

// The per-material block byte stride. Mirrors BindlessRegistry::MaterialParamStride, and moves only with it; one
// stride is shared across every material so a single ByteAddressBuffer holds a different
// per-shader parameter layout, read at index * MaterialParamStride.
static const uint MaterialParamStride = 1024;

// The canonical per-frame view-constants block (set 0, binding 5) is declared in
// view_constants.slang — the single source for the lighting, SSAO, and material shaders.
#include "../view_constants.slang"

// The ORM alpha channel's flag values and predicates, shared with the deferred lighting pass.
#include "surface_flags.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
// The byte-address storage-buffer bindless array (set 0, binding 3): each slot a whole
// game-supplied buffer a material reads typed at a byte offset
// (g_Buffers[params.Handle].Load<T>(off)), bound at full range and selected by handle
// index. The engine stays layout-agnostic (bytes + a slot); the shader owns the struct,
// mirroring the param block's own byte-addressed design. A material's handle is uniform
// across a draw (one material → one handle), so a load needs no NonUniformResourceIndex.
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;
// The 3D sampled-image bindless array (the volume set, set 1): a material samples a volume
// through a VolumeHandle slot, the 3D counterpart of g_Textures. Its own homogeneous set keeps
// a Texture3D descriptor out of set 0's Metal argument buffer on MoltenVK.
[[vk::binding(0, 1)]] Texture3D g_Volumes[];

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
};

// The per-draw data SSBO (set 3, binding 0 — the first author set, above the typed bindless
// registries at 0-2). Off the bindless registries — a closed producer→consumer per-frame buffer
// needs no global registration, and a dedicated set sidesteps the storage-buffer argument-buffer
// path on MoltenVK.
// Ring-buffered for frames-in-flight; the current frame's region base is folded into
// the candidate id by the pushed FrameBase, exactly as the material block selector is.
[[vk::binding(0, 3)]] StructuredBuffer<DrawData> g_DrawData;

DrawData LoadDrawData(uint candidateId)
{
    return g_DrawData[g_PC.FrameBase + candidateId];
}

// The deferred opaque g-buffer outputs the material fragment stage writes. The
// metallic-roughness channel set plus the screen-space motion vector and HDR emissive,
// funnelled through one struct: albedo into G0 (sRGB target, the sampler decodes to
// linear on read), the world-space normal into G1, packed occlusion/roughness/metallic
// into G2, the per-object motion vector into G3 (the TAA resolve reprojects through it),
// and the emissive radiance into G4. G3 and G4 are written every frame by the same
// surface pipeline, so motion vectors and emission cost no second geometry pass; with
// TAA off the velocity channel is written but unread.
//
// Emissive (G4) is linear HDR radiance the lighting pass adds to the pixel's outgoing
// light. It is non-negative (unsigned target), and magnitudes author in the HDR chain's
// exposure-scaled units (the same convention the lit chain, bloom, and tonemap use).
//
// ORM's alpha (G2.a) is the surface-flags channel: a material writes 0 there unless it wants
// one of the flags Veng/surface_flags.slang defines.
struct GBufferOutput
{
    float4 Albedo   : SV_Target0;  // rgb sRGB albedo; a unused
    float4 Normal   : SV_Target1;  // world-space normal in xyz
    float4 ORM      : SV_Target2;  // R occlusion, G roughness, B metallic, A surface flags
    float2 Velocity : SV_Target3;  // screen-space motion vector (curUV - prevUV)
    float3 Emissive : SV_Target4;  // linear HDR emissive radiance (non-negative)
};

// The same g-buffer set with a fragment-written depth. Returning this instead of GBufferOutput
// replaces the rasterized depth with the written value for both the depth test and the depth
// write, so a material whose drawn geometry does not stand where its shading says it does can
// sort by a value it computes instead. The targets are declared here rather than in the material
// so the two forms cannot drift apart.
//
// Two costs come with it. The draw loses early-Z rejection, since the depth is not known until
// the fragment has run. And the deferred lighting pass reconstructs world position from the depth
// attachment, so a written value moves that fragment's reconstructed position along its own view
// ray — the view vector is unchanged, a directional light is unaffected, and a positional light
// sees the fragment at the distance the depth names. Where that reconstructed position is a
// coordinate no geometry in the scene stands at, SurfaceFlagNoShadowReception in ORM's alpha
// declines the shadow-map lookups taken there.
struct GBufferDepthOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 ORM      : SV_Target2;  // R occlusion, G roughness, B metallic, A surface flags
    float2 Velocity : SV_Target3;
    float3 Emissive : SV_Target4;
    float  Depth    : SV_Depth;
};

// Carries a g-buffer result into the depth-writing form, so a fragment fills the targets exactly
// as it would without depth and names the depth once at the end.
GBufferDepthOutput WithDepth(GBufferOutput gbuffer, float depth)
{
    GBufferDepthOutput o;
    o.Albedo = gbuffer.Albedo;
    o.Normal = gbuffer.Normal;
    o.ORM = gbuffer.ORM;
    o.Velocity = gbuffer.Velocity;
    o.Emissive = gbuffer.Emissive;
    o.Depth = depth;
    return o;
}

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

// Unpacks a tangent-space normal from a sampled normal map, codec-agnostic across BC5,
// the ASTC XY convention, and a full-channel BC7/ASTC normal. Only X/Y are read from the
// sample (unsigned, *2-1); Z is reconstructed as sqrt(1 - x^2 - y^2). The two-channel
// codecs (BC5) and the ASTC XY convention store only X/Y and drop Z, so the reconstruct is
// mandatory there; a full-channel normal's stored Z is discarded and recomputed identically,
// so one helper serves every codec and a call site samples a normal without knowing its
// codec. The result is a unit tangent-space normal (+Z out of the surface).
float3 UnpackNormal(float4 sampled)
{
    const float2 xy = sampled.xy * 2.0 - 1.0;
    const float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    return float3(xy, z);
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
    // This draw's candidate id, so a fragment can identify its own draw and load its DrawData —
    // LoadDrawData(v_CandidateId) — to read the same per-instance record the vertex stage used
    // (e.g. its model matrix). Ignored by a fragment that does not need it.
    nointerpolation uint v_CandidateId : TEXCOORD4;
};
