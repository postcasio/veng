#include "Veng/translucent.slang"

// Scene-depth footprint checker (gpu fixture): checks the exact scene-depth reads against the copy
// texels this fragment covers, read here by their own definition — the Block-wide square at
// Block·sv_position, clamped inside the full valid extent — and returns 1 in each channel whose
// claims hold, at full coverage:
//   r — LoadSceneDepth is bit-equal to one of the footprint's texels;
//   g — SampleSceneDepthNearest / SampleSceneDepthFarthest are bit-equal to their largest and
//       smallest;
//   b — the farthest read's view depth is FarDepth over the far plane (+inf when FarDepth is 0),
//       and finite and positive elsewhere.
// Select.x = 1 returns white instead, a reference for the same pipeline.
struct MaterialParams
{
    float4 Select; // x the mode above, y Block (1 at full resolution, 2 in the half-res layer)
    float4 Expect; // x FarDepth (0: infinite), yz the full valid extent in pixels
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    if (p.Select.x > 0.5)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    Texture2D copy = g_Textures[vc.SceneColor.w];

    const int block = int(p.Select.y + 0.5);
    const int2 base = int2(input.sv_position.xy) * block;
    const int2 maxCoord = int2(p.Expect.yz + 0.5) - 1;
    float texels[4];
    float nearest = 0.0;
    float farthest = 1.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2 coord = min(base + int2(i & 1, i >> 1) * (block - 1), maxCoord);
        texels[i] = copy.Load(int3(coord, 0)).r;
        nearest = max(nearest, texels[i]);
        farthest = min(farthest, texels[i]);
    }

    const float2 uv = SceneScreenUV(vc, input.sv_position);
    const float2 extent = vc.ExtentParams.xy;
    const uint loaded = asuint(LoadSceneDepth(vc, uv));
    bool member = false;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        member = member || loaded == asuint(texels[i]);
    }
    const bool extremes = asuint(SampleSceneDepthNearest(vc, uv, extent)) == asuint(nearest) &&
                          asuint(SampleSceneDepthFarthest(vc, uv, extent)) == asuint(farthest);

    const float viewDepth = ViewDepthFromDeviceDepth(vc, SampleSceneDepthFarthest(vc, uv, extent));
    bool depthOk;
    if (farthest == 0.0)
    {
        depthOk = p.Expect.x == 0.0 ? (isinf(viewDepth) && viewDepth > 0.0)
                                    : abs(viewDepth - p.Expect.x) <= 1e-3 * p.Expect.x;
    }
    else
    {
        depthOk = !isinf(viewDepth) && !isnan(viewDepth) && viewDepth > 0.0;
    }
    return float4(member ? 1.0 : 0.0, extremes ? 1.0 : 0.0, depthOk ? 1.0 : 0.0, 1.0);
}
