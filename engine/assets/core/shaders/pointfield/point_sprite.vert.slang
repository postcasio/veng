// Point-field resolved-sprite vertex stage. Expands each point in one cull cell's range (a
// GpuFieldPoint in the resident SSBO at set 1 binding 0) into a camera-facing quad: six
// vertices selected by SV_VertexID, the corners offset along the camera's right/up axes (the
// View matrix's first two rows) scaled by the point's world size. The push FirstPoint offsets
// into the resident buffer so a per-cell draw reads only that cell's contiguous point run.
//
// The sprite is a constant-surface-brightness disc: its projected pixel size is clamped to
// [MinPixels, MaxPixels] and the vertex color is scaled by the flux-conserving square of the
// clamp ratio (capped at MaxIntensity), so a receding point dims like a real emitter and a
// near one concentrates into a small HDR-bright core instead of a huge disc.
#include "../view_constants.slang"

// One resident field point. Sixteen bytes, mirroring the CPU FieldPoint byte-for-byte.
struct GpuFieldPoint
{
    float3 Position;  // world-space position
    uint ColorRgba8;  // packed RGBA8 (R low byte)
    float Size;       // sprite edge length in world units
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuFieldPoint> g_Points;

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
    float2 v_ScreenUV : TEXCOORD1;
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
    // that pulls in the DrawParameters capability the device does not enable.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });

    // A non-instanced draw of 6*Count vertices: vertexId / 6 selects the point within the cell,
    // vertexId % 6 the quad corner. FirstPoint offsets into the resident buffer.
    uint localId = vertexId / 6;
    uint cornerId = vertexId % 6;

    GpuFieldPoint point = g_Points[g_PC.FirstPoint + localId];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Two-triangle quad corners in [-0.5, 0.5].
    static const float2 corners[6] = {
        float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
        float2(0.5, -0.5),  float2(0.5, 0.5),  float2(-0.5, 0.5),
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

    float3 worldPos = point.Position + worldSize * (corner.x * right + corner.y * up);
    float4 clip = mul(view.CurViewProj, float4(worldPos, 1.0));

    VSOutput output;
    output.sv_position = clip;
    output.v_UV = corner;
    output.v_Color = color;
    output.v_ScreenUV = (clip.xy / clip.w) * 0.5 + 0.5;
    output.v_NdcDepth = clip.z / clip.w;
    return output;
}
