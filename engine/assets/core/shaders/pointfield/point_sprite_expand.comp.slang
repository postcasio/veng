// Point-field sprite expansion + compaction. One compute dispatch per field per frame runs the
// per-point sprite work ONCE per point (projection, pixel-size clamp, flux gain, MaxIntensity cap,
// opacity fold) instead of once per corner vertex, and compacts out every point that contributes
// nothing before it costs a vertex invocation or a fragment.
//
// Threads cover the resolved-run point total. A thread maps to its point through the run table (a
// small SSBO of {FirstPoint, RunPointPrefix} runs uploaded per frame): a prefix-sum lookup finds
// the run whose prefix range contains the thread, then the point index is FirstPoint + (thread -
// prefix). Survivors — a point in front of the eye, whose gain·opacity-folded color exceeds a
// sub-epsilon floor, whose clamped quad touches the viewport — append a GpuSpriteRecord through an
// atomic cursor; the finalizer thread writes indexCount = 6·survivors into the single indirect
// command. Behind-eye, sub-epsilon, and fully-offscreen points write nothing.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this closed
// producer→consumer pass binds its point/run/record/args buffers on set 1 and takes all view
// state through push constants (no view-constants fetch), so it needs no bindless bind.

// One resident field point. Twenty bytes, mirroring the CPU FieldPoint byte-for-byte.
struct GpuFieldPoint
{
    float3 Position; // world-space position
    uint ColorRgba8; // packed RGBA8 (R low byte)
    float Size;      // sprite edge length in world units
};

// One resolved draw run: a contiguous point range [FirstPoint, FirstPoint+Count) drawn as sprites,
// plus the prefix sum of every earlier run's Count so a thread maps to its run by a prefix search.
struct GpuDrawRun
{
    uint FirstPoint;     // index of the run's first point in the resident buffer
    uint Count;          // number of points in the run
    uint PointPrefix;    // number of points in every run before this one (exclusive prefix sum)
    uint Pad;            // pad to 16 bytes
};

// One compacted sprite record. 32 bytes; matches the CPU GpuSpriteRecord and the sprite vertex
// stage byte-for-byte. HalfExtents/Color pack their float4-worth of data as f16 pairs — the direct
// path's interpolants are already f16-class on the target hardware, so the quantization is below
// the photometric thresholds the draw is compared against.
struct GpuSpriteRecord
{
    float4 ClipCenter; // projected sprite center (pre-divide)
    uint2 HalfExtents; // clip-space half-extent xy: right offset (2×f16), up offset (2×f16)
    uint2 Color;       // gain·opacity-folded HDR color: rg (2×f16), b + occluded-fade flag (2×f16)
};

// VkDrawIndexedIndirectCommand, the 20-byte record vkCmdDrawIndexedIndirect consumes. One per field.
struct DrawIndexedIndirectCommand
{
    uint IndexCount;    // 6 * survivors, finalized by the last thread
    uint InstanceCount; // preset to 1 by the CPU
    uint FirstIndex;    // preset to 0
    int VertexOffset;   // preset to 0
    uint FirstInstance; // preset to 0
};

// The resident points are a tightly-packed 20-byte stride (float3 + uint + float), matching the CPU
// FieldPoint byte-for-byte. Read through a ByteAddressBuffer at an explicit 20-byte stride so the
// element layout cannot drift from the CPU's on a std430 float3-alignment interpretation.
[[vk::binding(0, 1)]] ByteAddressBuffer g_Points;

static const uint FieldPointStride = 20;

GpuFieldPoint LoadPoint(uint index)
{
    uint base = index * FieldPointStride;
    GpuFieldPoint p;
    p.Position = asfloat(g_Points.Load3(base));
    p.ColorRgba8 = g_Points.Load(base + 12);
    p.Size = asfloat(g_Points.Load(base + 16));
    return p;
}
[[vk::binding(1, 1)]] StructuredBuffer<GpuDrawRun> g_Runs;
[[vk::binding(2, 1)]] RWStructuredBuffer<GpuSpriteRecord> g_Records;
// The args buffer is byte-addressed so IndexCount (word 0 of the VkDrawIndexedIndirectCommand) can
// be updated with a scalar atomic — an atomic on a StructuredBuffer struct member is not portable.
[[vk::binding(3, 1)]] RWByteAddressBuffer g_Args;
[[vk::binding(4, 1)]] RWStructuredBuffer<uint> g_Cursor; // [0] = survivor append cursor

struct PushConstants
{
    float4x4 ViewProj; // world → clip (this frame's camera)
    float4 ClipRight;  // ViewProj * float4(camera right, 0) — clip-space corner basis
    float4 ClipUp;     // ViewProj * float4(camera up, 0)
    uint RunCount;     // number of runs in g_Runs
    uint PointTotal;   // sum of every run's Count (the dispatch's thread span)
    float ProjScale;   // |Proj[1][1]| * viewportHeight / 2 — pixels per world at w = 1
    float MinPixels;   // sprite pixel-size clamp lower bound
    float MaxPixels;   // sprite pixel-size clamp upper bound
    float MaxIntensity; // gain / peak-brightness ceiling
    float Opacity;     // uniform display fade folded into the record color
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Packs two floats into a uint as a pair of IEEE-754 half-floats (x in the low 16 bits, y high).
uint PackHalf2(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

// Unpacks little-endian RGBA8 (R low byte) into linear [0,1] RGBA.
float4 UnpackRgba8(uint packed)
{
    return float4(float(packed & 0xFFu), float((packed >> 8) & 0xFFu),
                  float((packed >> 16) & 0xFFu), float((packed >> 24) & 0xFFu))
           / 255.0;
}

// Finds the run whose exclusive-prefix range [PointPrefix, PointPrefix+Count) contains thread,
// returning FirstPoint + (thread - PointPrefix): the resident point index for this thread. A
// linear scan — the run count is small (contiguous cull-cell runs, not per point).
uint PointIndexForThread(uint thread)
{
    for (uint r = 0; r < g_PC.RunCount; ++r)
    {
        GpuDrawRun run = g_Runs[r];
        if (thread < run.PointPrefix + run.Count)
        {
            return run.FirstPoint + (thread - run.PointPrefix);
        }
    }
    return 0;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint thread = id.x;
    if (thread >= g_PC.PointTotal)
    {
        return;
    }

    uint pidx = PointIndexForThread(thread);
    GpuFieldPoint point = LoadPoint(pidx);
    float4 clipCenter = mul(g_PC.ViewProj, float4(point.Position, 1.0));

    // Compact behind-eye points: they drew a degenerate quad in the direct path and contribute no
    // light. A zero-w center is likewise dropped (its projection is undefined).
    if (clipCenter.w <= 0.0)
    {
        return;
    }

    // The per-point sprite math, run once (the direct vertex stage runs it per corner). Projected
    // pixel size of the world disc at this depth, clamped, with flux-conserving gain capped at
    // MaxIntensity; worldSize scales to the clamped pixel size.
    float4 color = UnpackRgba8(point.ColorRgba8);
    float worldSize = point.Size;
    if (point.Size > 0.0)
    {
        float projPixels = point.Size * g_PC.ProjScale / clipCenter.w;
        float drawnPixels = clamp(projPixels, g_PC.MinPixels, g_PC.MaxPixels);
        float gain = min((projPixels * projPixels) / (drawnPixels * drawnPixels), g_PC.MaxIntensity);
        worldSize = point.Size * (drawnPixels / projPixels);
        color.rgb *= gain;
    }
    // Fold the point's alpha and the field opacity into rgb so the shared fragment stage — which
    // multiplies v_Color.rgb * v_Color.a * Opacity — matches when the compute-drawn record sets
    // v_Color.a = 1 and the raster path's push Opacity = 1.
    color.rgb *= color.a * g_PC.Opacity;

    // Compact sub-epsilon points: after gain·opacity the color rounds to nothing (flux conservation
    // dims a receding point quadratically), so its additive contribution is below the record's f16
    // floor and it would draw a fully-transparent quad.
    float peak = max(color.r, max(color.g, color.b));
    if (peak <= 1.0 / 2048.0)
    {
        return;
    }

    // The clip-space half-extent vectors: the corner FMA in the vertex stage is
    // clip = clipCenter + corner.xy * (halfRight, halfUp) with corner in [-1, 1] (the record folds
    // the point's own 0.5 half-corner into the extents, so the vertex stage does one FMA).
    float4 halfRight = (worldSize * 0.5) * g_PC.ClipRight;
    float4 halfUp = (worldSize * 0.5) * g_PC.ClipUp;

    // Compact fully-offscreen quads: the quad's clip-space AABB (center ± the two half-extents,
    // divided by w) misses the NDC box, so no corner rasterizes. Conservative — a corner behind the
    // near plane keeps the quad (its per-corner w may flip sign), matching the direct path.
    {
        float invW = 1.0 / clipCenter.w;
        float2 ndc = clipCenter.xy * invW;
        float2 ext = (abs(halfRight.xy) + abs(halfUp.xy)) * invW;
        float wSpan = abs(halfRight.w) + abs(halfUp.w);
        bool nearSafe = clipCenter.w - wSpan > 0.0;
        if (nearSafe && (ndc.x + ext.x < -1.0 || ndc.x - ext.x > 1.0 || ndc.y + ext.y < -1.0 ||
                         ndc.y - ext.y > 1.0))
        {
            return;
        }
    }

    // Survivor: append through the atomic cursor and write its record. The half-extents store only
    // the clip-space xy offset per corner (2×f16 each) — the raster-relevant part of the FMA; the
    // z/w skew of a screen-facing sprite's corner is negligible against the shared f16 quantization,
    // so the vertex stage reconstructs position xy per corner and inherits clipCenter's z/w.
    uint slot;
    InterlockedAdd(g_Cursor[0], 1u, slot);

    GpuSpriteRecord record;
    record.ClipCenter = clipCenter;
    record.HalfExtents = uint2(PackHalf2(halfRight.xy), PackHalf2(halfUp.xy));
    // Color rg / b, with the fourth f16 an occluded-fade flag (1 = no fade). The fragment stage owns
    // the per-pixel depth fade; this flag is the record's slot for a compute-side occlusion decision.
    float fade = 1.0;
    record.Color = uint2(PackHalf2(color.rg), PackHalf2(float2(color.b, fade)));

    g_Records[slot] = record;

    // Finalize the indirect draw's IndexCount (word 0 of the command) to 6·survivors: this survivor
    // owns record slot `slot`, so slots [0, slot] are all filled and the count is at least
    // (slot+1)·6. An atomic max over every survivor converges on 6·(highest slot + 1) = 6·survivors,
    // independent of thread order. A scalar byte-address atomic — portable where a struct-member
    // atomic is not.
    uint prev;
    g_Args.InterlockedMax(0, (slot + 1u) * 6u, prev);
}
