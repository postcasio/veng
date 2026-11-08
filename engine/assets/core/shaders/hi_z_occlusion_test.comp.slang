// Isolation compute pass proving the hi-Z occlusion test (IsOccluded) against a
// known scene. Each invocation owns one candidate: it reads the candidate's world
// AABB, runs IsOccluded against the supplied (previous-frame) pyramid + view-proj,
// and writes a per-candidate visibility flag (1 = visible/draw, 0 = occluded) to a
// result buffer a readback test reads. It issues no draw — it exists only to pin the
// test before the GPU cull wires it into indirect draw.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this
// closed test binds its candidate/result buffers and the pyramid on set 1.
#include "hi_z_occlusion.slang"
#include "Veng/binding.slang"

// A candidate's world-space AABB. xyz carry the corner; w is unused padding so the
// struct matches the 16-byte std430 element the test uploads.
struct Candidate
{
    float4 BoundsMin;
    float4 BoundsMax;
};

struct HiZOcclusionTestSet3
{
    Texture2D<float> HiZ;
    StructuredBuffer<Candidate> Candidates;
    RWStructuredBuffer<uint> Visibility;
};
VE_DESCRIPTOR_SET(3, HiZOcclusionTestSet3, g_Set3);

struct PushConstants
{
    float4x4 PrevViewProj;  // previous frame's camera world->clip
    uint2 HiZBaseExtent;    // mip 0 pixel extent of the pyramid
    uint CandidateCount;    // number of candidates to test
    float DepthBias;        // absorbs reduction quantization; a tie draws
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

    Candidate candidate = g_Set3.Candidates[index];
    bool occluded = IsOccluded(candidate.BoundsMin.xyz, candidate.BoundsMax.xyz,
                               g_PC.PrevViewProj, g_Set3.HiZ, g_PC.HiZBaseExtent, g_PC.DepthBias);

    g_Set3.Visibility[index] = occluded ? 0u : 1u;
}
