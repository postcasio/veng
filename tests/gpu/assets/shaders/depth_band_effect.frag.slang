// A PostProcessEffect fixture that samples the scene depth the engine binds and writes it as a
// grayscale ramp (reverse-Z: nearer geometry is brighter, the cleared far background is black). It
// proves the second input — depth — reaches the material, and that the depth read remaps through the
// render-scale sub-rect: at RenderScale < 1 with a temporal mode active the finished color is the
// full allocation while depth lives in the render sub-rect, so a shared UV would misread depth
// unless remapped. Sampling depth through DepthScaleUV keeps a near cube bright and the background
// dark at any render scale.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float4 DepthScaleUV; // xy depth valid-region scale, zw its half-texel clamp
    uint Depth;          // runtime-bound scene-depth source handle
    uint DepthSampler;   // runtime-bound scene-depth sampler handle
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float depth = SamplePostProcessDepth(p.Depth, p.DepthSampler, p.DepthScaleUV, input.v_UV);
    // Reverse-Z: any geometry has depth > 0, the cleared far background is exactly 0. Emit a
    // high-contrast near/far mask in red (robust to the small reverse-Z magnitude) and the raw
    // depth in green.
    float mask = depth > 0.0001 ? 1.0 : 0.0;
    return float4(mask, depth, 0.0, 1.0);
}
