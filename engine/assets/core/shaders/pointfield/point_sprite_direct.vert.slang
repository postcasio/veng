// Point-field resolved-sprite vertex stage. Expands each point in one cull cell's range (a
// GpuFieldPoint in the resident SSBO at set 1 binding 0) into a camera-facing quad: four unique
// vertices per point, indexed as two triangles by the pass's shared quad index buffer, the
// corners offset along the camera's right/up axes (the View matrix's first two rows) scaled by
// the point's world size. The push FirstPoint offsets into the resident buffer so a per-cell
// draw reads only that cell's contiguous point run.
//
// The sprite is a constant-surface-brightness disc: its projected pixel size is clamped to
// [MinPixels, MaxPixels] and the vertex color is scaled by the flux-conserving square of the
// clamp ratio (capped at MaxIntensity), so a receding point dims like a real emitter and a
// near one concentrates into a small HDR-bright core instead of a huge disc.
#include "../view_constants.slang"

// One resident field point. Twenty bytes, mirroring the CPU FieldPoint byte-for-byte.
struct GpuFieldPoint
{
    float3 Position;  // world-space position
    uint ColorRgba8;  // packed RGBA8 (R low byte)
    float Size;       // sprite edge length in world units
};

// The resident points are a tightly-packed 20-byte stride (float3 + uint + float), matching the CPU
// FieldPoint byte-for-byte. Read through a ByteAddressBuffer at an explicit 20-byte stride so the
// element layout cannot drift from the CPU's on a std430 float3-alignment interpretation (a
// StructuredBuffer<GpuFieldPoint> would round the float3 up to a 16-byte member and shear the read).
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

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
    uint ViewConstantsIndex;
    uint FirstPoint;
    uint PointCount;
    float MinPixels;
    float MaxPixels;
    float MaxIntensity;
    float ViewportWidth;
    float ViewportHeight;
    float Opacity;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_SampleUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
};

// Unpacks little-endian RGBA8 (R low byte) into linear [0,1] RGBA.
float4 UnpackRgba8(uint packed)
{
    return float4(float(packed & 0xFFu), float((packed >> 8) & 0xFFu),
                  float((packed >> 16) & 0xFFu), float((packed >> 24) & 0xFFu)) / 255.0;
}

[shader("vertex")]
VSOutput vsMain()
{
    // Read the VertexIndex builtin directly (not SV_VertexID) to avoid the base-vertex subtract
    // that pulls in the DrawParameters capability the device does not enable. The indexed draw
    // sets firstIndex=0 and vertexOffset=0, so the builtin equals the index buffer's value.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });

    // An indexed draw of 4 unique vertices per point (vertex id = 4*point + corner): vertexId / 4
    // selects the point within the cell, vertexId & 3 the quad corner. FirstPoint offsets into the
    // resident buffer. The shared index buffer expands the four corners into two triangles, so the
    // two shared corners hit the post-transform cache instead of re-shading.
    uint localId = vertexId / 4;
    uint cornerId = vertexId & 3;

    GpuFieldPoint point = LoadPoint(g_PC.FirstPoint + localId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // The four quad corners in [-0.5, 0.5], indexed by cornerId.
    static const float2 corners[4] = {
        float2(-0.5, -0.5),
        float2(0.5, -0.5),
        float2(-0.5, 0.5),
        float2(0.5, 0.5),
    };
    float2 corner = corners[cornerId];

    // Camera right/up are the View matrix's first two rows (the camera basis in world space).
    float3 right = float3(view.View[0][0], view.View[0][1], view.View[0][2]);
    float3 up = float3(view.View[1][0], view.View[1][1], view.View[1][2]);

    float4 clipCenter = mul(view.CurViewProj, float4(point.Position, 1.0));
    float4 color = UnpackRgba8(point.ColorRgba8);

    // Projected pixel size of the world-space disc at the point's depth: a world-space vertical
    // extent S spans S*|Proj[1][1]|/w of NDC, i.e. S*|Proj[1][1]|*H/(2w) pixels (the projection
    // is Y-flipped for Vulkan clip space, so the diagonal is negative).
    float worldSize = point.Size;
    if (clipCenter.w > 0.0 && point.Size > 0.0)
    {
        float projPixels = point.Size * abs(view.Proj[1][1]) * g_PC.ViewportHeight
                           / (2.0 * clipCenter.w);
        float drawnPixels = clamp(projPixels, g_PC.MinPixels, g_PC.MaxPixels);
        // Conserve flux across the clamp: brightness scales by the area ratio, capped so a very
        // near point cannot push unbounded HDR values into the accumulation target.
        float gain = min((projPixels * projPixels) / (drawnPixels * drawnPixels),
                         g_PC.MaxIntensity);
        worldSize = point.Size * (drawnPixels / projPixels);
        color.rgb *= gain;
    }

    // Offset the corner in clip space rather than projecting a second world-space position. The
    // world offset worldSize*(cx*right + cy*up) is linear in the corner, so its projection is
    // clipCenter + worldSize*(cx*ClipRight + cy*ClipUp) with the two clip-space basis vectors the
    // projected camera right/up axes (per-view constants the compiler hoists). One mat4 projection
    // and one FMA pair per vertex, algebraically identical to projecting the offset position.
    float4 clipRight = mul(view.CurViewProj, float4(right, 0.0));
    float4 clipUp = mul(view.CurViewProj, float4(up, 0.0));
    float4 clip = clipCenter + worldSize * (corner.x * clipRight + corner.y * clipUp);

    // Hoist the dynamic-resolution sub-rect remap here: the depth-fade fragment samples the
    // g-buffer depth at this interpolated sub-rect UV rather than re-fetching the view constants
    // and remapping per fragment. The remap is affine, so interpolating it adds no new
    // approximation over the already-linearly-interpolated screen UV.
    float2 screenUV = (clip.xy / clip.w) * 0.5 + 0.5;

    VSOutput output;
    output.sv_position = clip;
    output.v_UV = corner;
    output.v_Color = color;
    output.v_SampleUV = ScaledSampleUV(screenUV, view.RenderScaleUV.xy, view.MaxValidUV.xy);
    output.v_NdcDepth = clip.z / clip.w;
    return output;
}
