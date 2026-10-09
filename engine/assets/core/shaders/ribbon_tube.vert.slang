// Ribbon tube vertex stage. Expands each tube segment (set 3 binding 1) into a quad per side of its
// cross-section outline (the point table at set 3 binding 2): the side from outline point i to i + 1,
// swept from the start ring to the end ring. A ring places each outline point about its end's centre
// along that end's two axes, which already carry the trail's taper. Every segment is drawn at the
// most sides an outline has; a quad past its own outline's count collapses to one point, which the
// rasterizer discards. Positions are eye-relative and rotated into view space by the View matrix's
// rotation alone, as the band stage's are, and projected through the jittered Proj so the tube
// resolves under TAA with the scene.
#include "view_constants.slang"
#include "ribbon_common.slang"

// Mirrors the CPU GpuTrailTube the pass uploads (std430, 144 bytes).
struct GpuTrailTube
{
    float4 Start;      // xyz eye-relative start-ring centre
    float4 End;        // xyz eye-relative end-ring centre
    float4 StartColor; // rgb linear HDR, a opacity
    float4 EndColor;   // rgb linear HDR, a opacity
    float4 StartAxisX; // xyz the axis an outline x runs along at the start; w Softness
    float4 StartAxisY; // xyz the axis an outline y runs along at the start
    float4 EndAxisX;   // xyz the axis an outline x runs along at the end
    float4 EndAxisY;   // xyz the axis an outline y runs along at the end
    uint4 Outline;     // x the first outline point, y the point count
};

[[vk::binding(1, 3)]] StructuredBuffer<GpuTrailTube> g_Tubes;
[[vk::binding(2, 3)]] StructuredBuffer<float2> g_Outlines;

// The most sides a tube draws; matches the CPU MaxTrailOutlinePoints.
static const uint MaxOutlinePoints = 16;

struct TubeVSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    // The surface normal and the position, both in view space; the fragment fades by how squarely
    // the one faces the eye along the other.
    float3 v_Normal : TEXCOORD0;
    float3 v_ViewPosition : TEXCOORD1;
    // How sharply the glow falls off toward the silhouette.
    nointerpolation float v_Softness : TEXCOORD2;
};

float3 ToView(ViewConstants view, float3 v)
{
    return float3(dot(view.View[0].xyz, v), dot(view.View[1].xyz, v), dot(view.View[2].xyz, v));
}

// The outward normal at outline point i, smoothed across its two edges: perpendicular to the chord
// between its neighbours, turned away from the outline's origin. Carried through the ring's axes by
// their inverse transpose, which for two perpendicular axes divides each by its squared length.
float3 RingNormal(GpuTrailTube tube, uint i, uint count, float3 axisX, float3 axisY)
{
    float2 point = g_Outlines[tube.Outline.x + i];
    float2 chord = g_Outlines[tube.Outline.x + (i + 1) % count] -
                   g_Outlines[tube.Outline.x + (i + count - 1) % count];
    float2 normal = float2(chord.y, -chord.x);
    if (dot(normal, point) < 0.0)
    {
        normal = -normal;
    }
    float3 world = axisX * (normal.x / max(dot(axisX, axisX), 1e-12)) +
                   axisY * (normal.y / max(dot(axisY, axisY), 1e-12));
    return world;
}

// The VertexIndex builtin read directly (it includes the draw's firstVertex, which selects the
// alpha or additive run); SV_VertexID would pull in the DrawParameters capability.
uint LoadVertexIndex()
{
    return uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
}

[shader("vertex")]
TubeVSOutput vsMain()
{
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    uint vertexId = LoadVertexIndex();
    GpuTrailTube tube = g_Tubes[vertexId / (MaxOutlinePoints * 6)];
    uint local = vertexId % (MaxOutlinePoints * 6);
    uint side = local / 6;
    uint count = min(tube.Outline.y, MaxOutlinePoints);

    TubeVSOutput output;
    if (side >= count)
    {
        // Every vertex of an unused quad at one point: zero area, discarded.
        output.sv_position = float4(0.0, 0.0, 2.0, 1.0);
        output.v_Color = float4(0.0, 0.0, 0.0, 0.0);
        output.v_Normal = float3(0.0, 0.0, 1.0);
        output.v_ViewPosition = float3(0.0, 0.0, -1.0);
        output.v_Softness = 0.0;
        return output;
    }

    // Two-triangle quad corners: x runs start ring (0) to end ring (1), y this outline point (0)
    // to the next (1).
    static const uint2 corners[6] = {
        uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1),
    };
    uint2 corner = corners[local % 6];
    bool atEnd = corner.x == 1;
    uint pointIndex = (side + corner.y) % count;

    float3 center = atEnd ? tube.End.xyz : tube.Start.xyz;
    float3 axisX = atEnd ? tube.EndAxisX.xyz : tube.StartAxisX.xyz;
    float3 axisY = atEnd ? tube.EndAxisY.xyz : tube.StartAxisY.xyz;
    float2 point = g_Outlines[tube.Outline.x + pointIndex];
    float3 position = ToView(view, center + axisX * point.x + axisY * point.y);

    output.sv_position = mul(view.Proj, float4(position, 1.0));
    output.v_Color = atEnd ? tube.EndColor : tube.StartColor;
    output.v_Normal = ToView(view, RingNormal(tube, pointIndex, count, axisX, axisY));
    output.v_ViewPosition = position;
    output.v_Softness = tube.StartAxisX.w;
    return output;
}
