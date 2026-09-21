// A PostProcessEffect fixture that writes what its block holds rather than what it samples, so a
// frame's readback names the parameters the draw observed. Red is the depth valid-region scale the
// pass derives from the recording view's render extent — two viewports at different render scales
// are handed different values, so finding one value in both viewports' output means the draws read
// one shared block rather than each view's own. Green is a plain authored param, so a write to the
// material between frames is visible in the next readback.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float4 DepthScaleUV; // xy depth valid-region scale, zw its half-texel clamp
    float Tint;          // an authored param the pass never writes
};

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(g_PC.MaterialOffset);
    return float4(p.DepthScaleUV.x, p.Tint, 0.0, 1.0);
}
