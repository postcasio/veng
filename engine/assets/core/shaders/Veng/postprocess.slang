// The engine's PostProcess material contract: the set-0 bindless declarations a fullscreen
// effect reads, the per-frame view block, the post push block, and the screen-UV fragment
// input. A PostProcess material (fullscreen-triangle vertex stage, a single SV_Target0 color,
// invoked by the post chain) `#include`s this and declares its own MaterialParams beside it —
// the parameter block is per-shader by definition (the cooker reflects each shader's own
// struct to pack its fields at the reflected offsets), so it is not part of the engine
// contract. The Surface and Sky domains have their own contracts in Veng/surface.slang and
// Veng/sky.slang.
//
// A PostProcess material reuses the engine's fullscreen vertex stage (fullscreen.vert), so
// only its fragment declares a push block. It declares exactly one push_constant block, so it
// does not include Veng/surface.slang, whose g_PC (the surface push) would collide with it.

// The per-material block byte stride. Mirrors BindlessRegistry::MaterialParamStride, and moves only with it; one
// stride is shared across every material so a single ByteAddressBuffer holds a different
// per-shader parameter layout, read at index * MaterialParamStride.
static const uint MaterialParamStride = 1024;

// The canonical per-frame view-constants block (set 0, binding 5) is declared in
// view_constants.slang — the single source for the lighting, SSAO, and material shaders.
#include "../view_constants.slang"

// The set-0 bindless arrays a PostProcess effect reads: the sampled images + samplers (an
// upstream target is runtime-bound as a handle field), the byte-address storage-buffer array
// (a data-driven effect reads a game-supplied buffer by handle), and the per-material param
// block. Mirrors the BindlessRegistry layout.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;
// The 3D sampled-image bindless array (the volume set, set 1): a material samples a volume
// through a VolumeHandle slot, the 3D counterpart of g_Textures. Its own homogeneous set keeps
// a Texture3D descriptor out of set 0's Metal argument buffer on MoltenVK.
[[vk::binding(0, 1)]] Texture3D g_Volumes[];

// A PostProcess fragment's per-draw selector sits at push-constant offset 0 (no geometry
// block precedes it, unlike a Surface shader's MVP). A PostProcess fragment reads its
// material index here rather than from a vertex interpolant.
struct PostProcessPushConstants
{
    uint MaterialIndex; // per-material param-block selector (frame-folded)
};

[[vk::push_constant]] ConstantBuffer<PostProcessPushConstants> g_PC;

// The screen-UV interpolant a PostProcess fragment reads, written by the fullscreen vertex
// stage (fullscreen.vert). A PostProcess fragment's fsMain signature takes this struct and
// reads its material selector from g_PC, since it has no per-draw v_MaterialIndex interpolant.
struct PostProcessFragmentInput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};
