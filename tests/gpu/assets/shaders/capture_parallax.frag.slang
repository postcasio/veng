#include "Veng/translucent.slang"
#include "Veng/probedepth.slang"

// A capture-consuming material for the gpu band: it reads the probe centre the component publishes,
// AND performs the distance-map march it is named after. What it needs from the engine is the world
// position the map was rendered from, a flag saying whether a capture exists at all, and — for the
// march — the octahedral distance map and its point sampler, and this material makes all of them
// readable off a rendered frame.
//
// Branches, selected by Display:
//  - Display.w set: MARCH. CaptureDepthCorrect walks an authored ray (RayOrigin measured from the
//    probe centre, RayDir a direction, both in the capture's frame; RayDir.w the step budget, 0 =
//    the module default) against the Depth/DepthSampler distance map and returns the corrected
//    direction. A direction is signed and the terminal tonemap clamps its input, so the corrected
//    direction is ENCODED into [0,1] as normalize(dir) * 0.5 + 0.5; the test decodes v * 2 - 1 and
//    renders under the None tonemapper (Exposure 1), so the encoding round-trips through saturate.
//  - otherwise the plan-00 centre/frame branches, in the order a real consumer takes them: an
//    unbound centre (w == 0) returns bright magenta — the only branch that may run while the handle
//    slots hold an unpopulated sentinel; Display.x the published centre as radiance; Display.y and
//    Display.z the published frame's imaginary and real parts; else it samples the capture in -Y.
struct MaterialParams
{
    float4 Center;      // engine-published: xyz the probe's world position, w 1 while a capture is bound
    float4 Orientation; // engine-published: the capture's frame as a quaternion, xyz then w
    float4 Display;     // selects what the fragment shows: x centre, y frame xyz, z frame w, w march
    float4 RayOrigin;   // march: probe-relative ray origin, in the capture's frame
    float4 RayDir;      // march: ray direction (xyz) in the capture's frame; w the step budget
    uint  Texture;
    uint  Sampler;
    uint  Depth;        // engine-bound: the octahedral distance map
    uint  DepthSampler; // engine-bound: the point clamp sampler
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    // The march branch is first: it authors its own ray and does not read the centre, so it runs
    // whether or not CenterSlot is bound.
    if (p.Display.w > 0.5)
    {
        const int steps = int(p.RayDir.w);
        const float3 corrected =
            steps > 0
                ? CaptureDepthCorrect(p.RayOrigin.xyz, p.RayDir.xyz,
                                      g_Textures[NonUniformResourceIndex(p.Depth)],
                                      g_Samplers[NonUniformResourceIndex(p.DepthSampler)], steps)
                : CaptureDepthCorrect(p.RayOrigin.xyz, p.RayDir.xyz,
                                      g_Textures[NonUniformResourceIndex(p.Depth)],
                                      g_Samplers[NonUniformResourceIndex(p.DepthSampler)]);
        return float4(normalize(corrected) * 0.5 + 0.5, 1.0);
    }

    // Nothing bound: the texture slot carries the unbound sentinel, so this is the only branch that
    // may sample nothing. Bright, so the frame reads it whatever the exposure.
    if (p.Center.w <= 0.5)
    {
        return float4(2.0, 0.0, 2.0, 1.0);
    }

    if (p.Display.x > 0.5)
    {
        return float4(p.Center.xyz, 1.0);
    }

    if (p.Display.y > 0.5)
    {
        return float4(p.Orientation.xyz, 1.0);
    }

    if (p.Display.z > 0.5)
    {
        return float4(p.Orientation.www, 1.0);
    }

    const float2 uv = OctahedralUV(float3(0.0, -1.0, 0.0));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
