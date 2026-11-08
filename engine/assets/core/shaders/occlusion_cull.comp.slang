// GPU occlusion cull → indirect draw. One invocation per camera-frustum survivor the
// broadphase uploaded: it runs the hi-Z occlusion test (IsOccluded) against the
// previous frame's pyramid. The compute does NOT re-run frustum culling — the BVH already did
// that CPU-side; this adds only occlusion. Its verdict reaches the draws one of two ways:
//
// - Fixed maximum (Compact 0): it writes its own command slot's instanceCount (1 = survivor,
//   0 = culled), and the geometry pass issues every slot of a group, a zero-instance command
//   drawing nothing — for a device without count-driven indirect draws.
// - Compacted (Compact 1): a survivor appends its command to the front of its group's slot run
//   and counts itself into the group's draw count, and the geometry pass draws that many.
//
// When HistoryValid is 0 (first frame, post-resize/configure, or a large view delta)
// every candidate is treated visible (frustum-only that frame), so a stale or absent
// pyramid can only leave a draw in, never wrongly drop a visible one.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this
// closed producer→consumer pass binds its candidate/command/count buffers and the
// pyramid on set 3.
#include "hi_z_occlusion.slang"
#include "Veng/binding.slang"

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
    uint   GroupFirst;    // the first slot of the candidate's draw group
    uint   Pad0;
    uint   Pad1;
    uint   Pad2;
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

struct OcclusionCullSet3
{
    Texture2D<float> HiZ;
    StructuredBuffer<CullCandidate> Candidates;
    RWStructuredBuffer<DrawIndexedIndirectCommand> Commands;
    RWStructuredBuffer<uint> DrawCount;  // [CountIndex] the survivor stat, then each group's count
};
VE_DESCRIPTOR_SET(3, OcclusionCullSet3, g_Set3);

struct PushConstants
{
    float4x4 PrevViewProj; // previous frame's camera world->clip (the pyramid is its depth)
    uint2 HiZBaseExtent;   // mip 0 pixel extent of the pyramid
    uint CandidateCount;   // number of uploaded candidates to test
    uint HistoryValid;     // 1 = test occlusion; 0 = frustum-only (all visible)
    float DepthBias;       // absorbs reduction quantization; a tie draws
    uint FrameBase;        // candidate/command region base (currentFrame * MaxCandidates)
    uint CountIndex;       // this frame's survivor stat in the count buffer; group counts follow
    uint Compact;          // 1 compacts survivors to the front of their group's run
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

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

    CullCandidate candidate = g_Set3.Candidates[slot];

    bool occluded = false;
    if (g_PC.HistoryValid != 0u)
    {
        occluded = IsOccluded(candidate.BoundsMin.xyz, candidate.BoundsMax.xyz, g_PC.PrevViewProj,
                              g_Set3.HiZ, g_PC.HiZBaseExtent, g_PC.DepthBias);
    }

    uint instanceCount = occluded ? 0u : 1u;
    if (g_PC.Compact != 0u)
    {
        if (occluded)
        {
            return;
        }
        uint position = 0u;
        InterlockedAdd(g_Set3.DrawCount[g_PC.CountIndex + 1u + candidate.GroupFirst], 1u, position);
        slot = g_PC.FrameBase + candidate.GroupFirst + position;
    }

    DrawIndexedIndirectCommand command;
    command.IndexCount = candidate.IndexCount;
    command.InstanceCount = instanceCount;
    command.FirstIndex = candidate.FirstIndex;
    command.VertexOffset = candidate.VertexOffset;
    command.FirstInstance = candidate.FirstInstance;
    g_Set3.Commands[slot] = command;

    // Survivor count for the debug stat only (read back next frame, never gating the draw).
    if (instanceCount != 0u)
    {
        InterlockedAdd(g_Set3.DrawCount[g_PC.CountIndex], 1u);
    }
}
