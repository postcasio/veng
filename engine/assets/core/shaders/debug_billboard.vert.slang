// Debug-draw billboard vertex stage. Expands each instance (a GPU billboard record in
// the per-frame SSBO at set 1 binding 0) into a camera-facing quad: six vertices selected
// by SV_VertexID, the corners offset along the camera's right/up axes (the View matrix's
// first two rows) scaled by the billboard's size. Outputs the quad UV, color, the logical
// screen UV, and the fragment's NDC depth for the depth-aware occlusion fade.
#include "view_constants.slang"

// One GPU billboard record. vec4 fields first (std430 packs them at 16-byte boundaries);
// Texture + padding trail. Mirrors the CPU GpuBillboard the pass uploads.
struct GpuBillboard
{
    float4 PositionSize;  // xyz world center, w edge length in world units
    float4 Color;         // linear RGBA tint
    uint Texture;         // icon bindless index
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuBillboard> g_Billboards;

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
    uint ViewConstantsIndex;
    uint Pad0;
    float OccludedFade;
    uint Pad1;
    uint Pad2;
    uint Pad3;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
    nointerpolation uint v_Texture : TEXCOORD3;
};

[shader("vertex")]
VSOutput vsMain()
{
    // Read the SPIR-V VertexIndex builtin directly rather than SV_VertexID: the latter subtracts
    // the base-vertex offset and so pulls in the DrawParameters capability, which the device does
    // not enable (the same reason fullscreen.vert reads it this way).
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });

    // A single non-instanced draw of 6*N vertices: vertexId / 6 selects the billboard record,
    // vertexId % 6 the quad corner — no instance attribute, no SV_InstanceID.
    uint instanceId = vertexId / 6;
    uint cornerId = vertexId % 6;

    GpuBillboard bb = g_Billboards[instanceId];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Two-triangle quad corners (0,0)(1,0)(0,1) / (1,0)(1,1)(0,1) in [-0.5, 0.5].
    static const float2 corners[6] = {
        float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
        float2(0.5, -0.5),  float2(0.5, 0.5),  float2(-0.5, 0.5),
    };
    float2 corner = corners[cornerId];

    // Camera right/up are the View matrix's first two rows (world axes of the view basis):
    // View transforms world → view (used as mul(View, worldPos) everywhere), so its rows are
    // the camera basis in world space. View[0]/View[1] index those rows directly.
    float3 right = float3(view.View[0][0], view.View[0][1], view.View[0][2]);
    float3 up = float3(view.View[1][0], view.View[1][1], view.View[1][2]);

    float3 worldPos = bb.PositionSize.xyz + bb.PositionSize.w * (corner.x * right + corner.y * up);
    float4 clip = mul(view.CurViewProj, float4(worldPos, 1.0));

    VSOutput output;
    output.sv_position = clip;
    // Quad UV: corner (-0.5..0.5) → (0..1); flip V so the texture is upright.
    output.v_UV = float2(corner.x + 0.5, 0.5 - corner.y);
    output.v_Color = bb.Color;
    output.v_ScreenUV = (clip.xy / clip.w) * 0.5 + 0.5;
    output.v_NdcDepth = clip.z / clip.w;
    output.v_Texture = bb.Texture;
    return output;
}
