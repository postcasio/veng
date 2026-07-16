// Point-field aggregate-splat vertex stage. Expands each per-cell density record (a
// GpuAggregateSplat in the per-frame SSBO at set 1 binding 0) into a screen-space quad centered
// on the cell's projected point centroid, sized in pixels (converted to clip via the pushed
// viewport extent), so a dense/distant cell reads as one smooth additive glow rather than
// resolved points. Four unique vertices per splat, indexed into two triangles by the pass's
// shared quad index buffer.
#include "../view_constants.slang"

// One aggregate splat record: world point centroid + quad size in pixels (the fragment
// kernel's whole three-cell support), then the cell's flux-normalized per-pixel color (the CPU
// spreads the cell's summed flux over its projected cell area).
struct GpuAggregateSplat
{
    float4 CenterSize; // xyz world point centroid, w quad size in pixels
    float4 Color;      // flux-normalized per-pixel color (rgb), HDR
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuAggregateSplat> g_Splats;

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
    float Spikes;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_Color : COLOR0;
};

[shader("vertex")]
VSOutput vsMain()
{
    // The indexed draw sets firstIndex=0 and vertexOffset=0, so the VertexIndex builtin equals the
    // index buffer's value: 4 unique vertices per splat (vertex id = 4*splat + corner), expanded
    // into two triangles by the shared index buffer so the two shared corners hit the cache.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
    uint localId = vertexId / 4;
    uint cornerId = vertexId & 3;

    GpuAggregateSplat splat = g_Splats[localId];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    static const float2 corners[4] = {
        float2(-0.5, -0.5),
        float2(0.5, -0.5),
        float2(-0.5, 0.5),
        float2(0.5, 0.5),
    };
    float2 corner = corners[cornerId];

    // Project the centroid, then offset the corner in clip space by the pixel size scaled to NDC
    // (a pixel is 2/extent of clip width per w). This keeps the splat a constant screen size.
    float4 clip = mul(view.CurViewProj, float4(splat.CenterSize.xyz, 1.0));
    float2 pixelToClip = float2(2.0 / g_PC.ViewportWidth, 2.0 / g_PC.ViewportHeight);
    clip.xy += corner * splat.CenterSize.w * pixelToClip * clip.w;

    VSOutput output;
    output.sv_position = clip;
    output.v_UV = corner;
    output.v_Color = splat.Color.rgb;
    return output;
}
