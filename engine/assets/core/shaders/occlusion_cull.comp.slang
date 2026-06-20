// GPU occlusion cull → indirect draw. One invocation per camera-frustum survivor the
// broadphase uploaded: it runs the hi-Z occlusion test (IsOccluded) against the
// previous frame's pyramid and writes its VkDrawIndexedIndirectCommand's instanceCount
// (1 = survivor / 0 = culled). A zero-instance command no-ops, so the geometry pass
// issues one vkCmdDrawIndexedIndirect over the fixed candidate maximum with no
// GPU-sourced count (MoltenVK lacks drawIndirectCount). The compute does NOT re-run
// frustum culling — the BVH already did that CPU-side; this adds only occlusion.
//
// When HistoryValid is 0 (first frame, post-resize/configure, or a large view delta)
// every candidate is treated visible (frustum-only that frame), so a stale or absent
// pyramid can only leave a draw in, never wrongly drop a visible one.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this
// closed producer→consumer pass binds its candidate/command/count buffers and the
// pyramid on set 1.
#include "hi_z_occlusion.slang"

// One uploaded camera-frustum survivor: its world AABB plus the indexed-draw args the
// command needs. std430-identical to the engine's GpuCullCandidate.
struct CullCandidate
{
    float4 BoundsMin;     // world-space submesh AABB min (xyz; w pad)
    float4 BoundsMax;     // world-space submesh AABB max (xyz; w pad)
    uint   IndexCount;    // VkDrawIndexedIndirectCommand.indexCount
    uint   FirstIndex;    // .firstIndex
    int    VertexOffset;  // .vertexOffset
    uint   FirstInstance; // .firstInstance — the candidate id the vertex stage reads
};

// VkDrawIndexedIndirectCommand, the 20-byte record vkCmdDrawIndexedIndirect consumes.
struct DrawIndexedIndirectCommand
{
    uint IndexCount;
    uint InstanceCount;
    uint FirstIndex;
    int  VertexOffset;
    uint FirstInstance;
};

[[vk::binding(0, 1)]] Texture2D<float> g_HiZ;
[[vk::binding(1, 1)]] StructuredBuffer<CullCandidate> g_Candidates;
[[vk::binding(2, 1)]] RWStructuredBuffer<DrawIndexedIndirectCommand> g_Commands;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> g_DrawCount; // [0] survivor count for the stat

struct PushConstants
{
    float4x4 PrevViewProj; // previous frame's camera world->clip (the pyramid is its depth)
    uint2 HiZBaseExtent;   // mip 0 pixel extent of the pyramid
    uint CandidateCount;   // number of uploaded candidates to test
    uint HistoryValid;     // 1 = test occlusion; 0 = frustum-only (all visible)
    float DepthBias;       // absorbs reduction quantization; a tie draws
    uint FrameBase;        // candidate/command region base (currentFrame * MaxCandidates)
    uint CountIndex;       // this frame's slot in the survivor-count buffer
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= g_PC.CandidateCount)
    {
        return;
    }

    // The candidate/command buffers are ring-buffered for frames-in-flight; fold the
    // frame's region base into the index, exactly as the per-draw DrawData buffer is.
    uint slot = g_PC.FrameBase + index;

    CullCandidate candidate = g_Candidates[slot];

    bool occluded = false;
    if (g_PC.HistoryValid != 0u)
    {
        occluded = IsOccluded(candidate.BoundsMin.xyz, candidate.BoundsMax.xyz, g_PC.PrevViewProj,
                              g_HiZ, g_PC.HiZBaseExtent, g_PC.DepthBias);
    }

    uint instanceCount = occluded ? 0u : 1u;

    DrawIndexedIndirectCommand command;
    command.IndexCount = candidate.IndexCount;
    command.InstanceCount = instanceCount;
    command.FirstIndex = candidate.FirstIndex;
    command.VertexOffset = candidate.VertexOffset;
    command.FirstInstance = candidate.FirstInstance;
    g_Commands[slot] = command;

    // Survivor count for the debug stat only (read back next frame, never gating the draw).
    if (instanceCount != 0u)
    {
        InterlockedAdd(g_DrawCount[g_PC.CountIndex], 1u);
    }
}
