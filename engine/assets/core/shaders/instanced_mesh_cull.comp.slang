// Per-instance cull and level-of-detail selection for the frame's instanced mesh sets. One dispatch
// covers every (view, set) job of the frame — the main camera and each shadow view, for every set
// that view does not reject whole — one invocation per instance of each job. An invocation tests its
// instance's bounds against the view's frustum, its projected size in the main camera against the
// set's pixel band, a shadow view's minimum caster size, and (main view, occlusion on) last frame's
// hi-Z pyramid; a survivor selects its level and appends its instance index to that level's list,
// counting itself into the level's indirect draws. The renderer wrote every command's index range
// and zeroed its instance count beforehand.
//
// The CPU fallback (src/Renderer/InstancedCull.cpp) runs the same decisions on the host; the two
// are kept alike.
#include "hi_z_occlusion.slang"
#include "Veng/instanced_mesh_math.slang"

// The frame's shared inputs, at the start of g_Tables.
struct CullFrame
{
    float4x4 ReferenceViewProj; // the main camera's unjittered world -> clip (the size band)
    float4x4 PrevViewProj;      // last frame's camera world -> clip (the hi-Z pyramid's)
    float4 ReferenceEye;        // xyz the main camera's position, w its rendered height in pixels
    uint2 HiZExtent;            // the pyramid's level-0 extent
    uint HiZTexture;            // the pyramid's bindless texture slot
    uint OcclusionValid;        // 1 when the main view tests against the pyramid this frame
    float DepthBias;            // absorbs reduction quantization; a tie draws
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

// One (view, set) pair the dispatch covers: a contiguous run of invocations, one per instance.
struct CullJob
{
    uint ThreadBase;  // the job's first invocation
    uint Set;         // index into the set table
    uint View;        // index into the view table
    uint CommandBase; // the job's first indirect command; level l, submesh s is l * SubMeshes + s
    uint ListBase;    // word index of level 0's list; level l's starts Count words further per level
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

// One set: its frame, its mesh's bound and levels, and its per-instance tests.
struct CullSet
{
    float4x4 World;      // set frame -> world
    float4 BoundsMin;    // the mesh's level-0 bound (xyz)
    float4 BoundsMax;
    float4 LodLow;       // level switch thresholds 0..3
    float4 LodHigh;      // thresholds 4..6
    uint RecordBase;     // record index of the set's first instance
    uint Count;          // the set's instance count
    uint LodCount;       // levels the mesh carries
    uint SubMeshCount;   // submeshes the mesh carries
    float MinPixels;     // the projected-size band in the reference view
    float MaxPixels;
    float MinShadowTexels; // a shadow view skips a caster projecting smaller
    int ForcedLod;       // the set's forced level, or -1
    uint LodMemoryBase;  // the set's first entry in g_LodMemory
    uint HistoryValid;   // 1 when g_LodMemory holds last frame's levels for this set
    uint Pad0;
    uint Pad1;
};

// The view flags.
static const uint ViewMain = 1u;        // the main camera: keeps level memory, may test occlusion
static const uint ViewFrustumCull = 2u; // instances outside Planes are culled
static const uint ViewPerspective = 4u; // the level view is a perspective one

// One view: where its casters or draws are culled and how it measures size.
struct CullView
{
    float4 Planes[6];      // inward frustum planes, world space
    float4 Eye;            // xyz the level view's eye, w its projection scale
    float LodScreenScale;  // the level view's size multiplier (bias and screen fraction)
    float Hysteresis;      // the level band's half-width; 0 for a view with no level memory
    uint Flags;
    float Resolution;      // a shadow view's tile size in texels
    int ForcedLod;         // the view's forced level, or -1
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

struct DrawIndexedIndirectCommand
{
    uint IndexCount;
    uint InstanceCount;
    uint FirstIndex;
    int VertexOffset;
    uint FirstInstance;
};

[[vk::binding(0, 0)]] Texture2D g_Textures[];

[[vk::binding(0, 3)]] RWByteAddressBuffer g_Data;
[[vk::binding(1, 3)]] RWStructuredBuffer<DrawIndexedIndirectCommand> g_Commands;
[[vk::binding(2, 3)]] RWStructuredBuffer<uint> g_LodMemory;
[[vk::binding(3, 3)]] ByteAddressBuffer g_Tables;

struct PushConstants
{
    uint JobsOffset;   // byte offset of the job table in g_Tables
    uint SetsOffset;   // byte offset of the set table
    uint ViewsOffset;  // byte offset of the view table
    uint JobCount;
    uint ThreadCount;  // invocations the jobs cover
    uint RowThreads;   // invocations per dispatch row (groups in x times 64)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const uint CullJobBytes = 32u;
static const uint CullSetBytes = 176u;
static const uint CullViewBytes = 144u;

// The planes are read from the table by the view's offset rather than taken in a CullView, and the
// occlusion test takes the frame's fields: the Slang compiler crashes on a function taking a struct
// that was loaded from a byte-address buffer.
bool InFrustum(uint viewOffset, float3 boundsMin, float3 boundsMax)
{
    [unroll]
    for (uint p = 0u; p < 6u; ++p)
    {
        const float4 plane = g_Tables.Load<float4>(viewOffset + p * 16u);
        const float3 positive = float3(plane.x >= 0.0 ? boundsMax.x : boundsMin.x,
                                       plane.y >= 0.0 ? boundsMax.y : boundsMin.y,
                                       plane.z >= 0.0 ? boundsMax.z : boundsMin.z);
        if (dot(plane.xyz, positive) + plane.w < 0.0)
        {
            return false;
        }
    }
    return true;
}

bool IsHiZOccluded(float4x4 prevViewProj, uint2 hiZExtent, uint hiZTexture, float depthBias,
                   float3 boundsMin, float3 boundsMax)
{
    const HiZFootprint footprint =
        ComputeHiZFootprint(boundsMin, boundsMax, prevViewProj, hiZExtent);
    if (!footprint.Testable)
    {
        return false;
    }
    float dFar = 1.0;
    [unroll]
    for (int ty = 0; ty <= 1; ++ty)
    {
        [unroll]
        for (int tx = 0; tx <= 1; ++tx)
        {
            dFar = min(dFar, g_Textures[hiZTexture]
                                 .Load(int3(HiZFootprintTexel(footprint, tx, ty),
                                            int(footprint.Level)))
                                 .r);
        }
    }
    return IsFootprintOccluded(footprint, dFar, depthBias);
}

// The job whose invocation run holds invocation: the last whose ThreadBase is at or below it.
// (Not named "thread", which is a Metal address-space keyword the shader is translated into.)
uint FindJob(uint invocation)
{
    uint low = 0u;
    uint high = g_PC.JobCount;
    while (high - low > 1u)
    {
        const uint mid = (low + high) / 2u;
        const uint base = g_Tables.Load(g_PC.JobsOffset + mid * CullJobBytes);
        if (base <= invocation)
        {
            low = mid;
        }
        else
        {
            high = mid;
        }
    }
    return low;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const uint invocation = id.y * g_PC.RowThreads + id.x;
    if (invocation >= g_PC.ThreadCount)
    {
        return;
    }

    const CullFrame frame = g_Tables.Load<CullFrame>(0u);
    const CullJob job = g_Tables.Load<CullJob>(g_PC.JobsOffset + FindJob(invocation) * CullJobBytes);
    const CullSet set = g_Tables.Load<CullSet>(g_PC.SetsOffset + job.Set * CullSetBytes);
    const uint viewOffset = g_PC.ViewsOffset + job.View * CullViewBytes;
    const CullView view = g_Tables.Load<CullView>(viewOffset);
    const uint instance = invocation - job.ThreadBase;
    if (instance >= set.Count)
    {
        return;
    }

    const InstanceRecord record =
        g_Data.Load<InstanceRecord>((set.RecordBase + instance) * InstanceRecordBytes);
    const float4x4 model = mul(set.World, InstanceMatrix(record));
    float3 center;
    float radius;
    InstanceBoundingSphere(model, set.BoundsMin.xyz, set.BoundsMax.xyz, center, radius);
    float3 worldMin;
    float3 worldMax;
    InstanceWorldBounds(model, set.BoundsMin.xyz, set.BoundsMax.xyz, worldMin, worldMax);

    const bool isMain = (view.Flags & ViewMain) != 0u;
    const uint memory = set.LodMemoryBase + instance;

    bool culled = (view.Flags & ViewFrustumCull) != 0u && !InFrustum(viewOffset, worldMin, worldMax);
    if (!culled)
    {
        // The band is measured in the main camera in every view, so a shadow view casts exactly
        // the instances the main view's band keeps.
        const float pixels = ReferenceProjectedPixels(frame.ReferenceViewProj,
                                                      frame.ReferenceEye.xyz,
                                                      frame.ReferenceEye.w, center, radius);
        culled = pixels < set.MinPixels || pixels > set.MaxPixels;
    }
    if (!culled && !isMain && set.MinShadowTexels > 0.0)
    {
        const float texels =
            ProjectedSphereSize(view.Eye.xyz, view.Eye.w, (view.Flags & ViewPerspective) != 0u,
                                1.0, center, radius) *
            view.Resolution;
        culled = texels < set.MinShadowTexels;
    }
    if (!culled && isMain && frame.OcclusionValid != 0u)
    {
        culled = IsHiZOccluded(frame.PrevViewProj, frame.HiZExtent, frame.HiZTexture,
                               frame.DepthBias, worldMin, worldMax);
    }
    if (culled)
    {
        if (isMain)
        {
            g_LodMemory[memory] = InstanceNoPreviousLod;
        }
        return;
    }

    uint lod = 0u;
    if (set.LodCount > 1u)
    {
        const int forced = set.ForcedLod >= 0 ? set.ForcedLod : view.ForcedLod;
        if (forced >= 0)
        {
            lod = min(uint(forced), set.LodCount - 1u);
        }
        else
        {
            uint previous = InstanceNoPreviousLod;
            if (isMain && set.HistoryValid != 0u)
            {
                previous = g_LodMemory[memory];
                previous = previous == InstanceNoPreviousLod ? previous
                                                             : min(previous, set.LodCount - 1u);
            }
            const float size =
                ProjectedSphereSize(view.Eye.xyz, view.Eye.w, (view.Flags & ViewPerspective) != 0u,
                                    view.LodScreenScale, center, radius);
            lod = min(SelectInstanceLod(set.LodLow, set.LodHigh, set.LodCount - 1u, size, previous,
                                        view.Hysteresis),
                      set.LodCount - 1u);
        }
    }
    if (isMain)
    {
        g_LodMemory[memory] = lod;
    }

    // Every submesh of a level draws the level's one list, so each counts the survivor; the first
    // one's count hands out the list slot.
    const uint commandBase = job.CommandBase + lod * set.SubMeshCount;
    uint slot = 0u;
    InterlockedAdd(g_Commands[commandBase].InstanceCount, 1u, slot);
    for (uint s = 1u; s < set.SubMeshCount; ++s)
    {
        InterlockedAdd(g_Commands[commandBase + s].InstanceCount, 1u);
    }
    g_Data.Store((job.ListBase + lod * set.Count + slot) * 4u, instance);
}
