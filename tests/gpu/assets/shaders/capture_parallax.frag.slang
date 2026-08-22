#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"
#include "Veng/probeproxy.slang"

// A capture-consuming material that reads the probe centre the component publishes, and performs
// the proxy correction a consuming material may include (gpu fixture). What it needs from the
// engine is the world position the map was rendered from and a flag saying whether a capture exists
// at all, and this material makes both readable off a rendered frame — as it makes the corrected
// direction readable, which is the only way a rendered frame can observe the correction at all: the
// ordinary sampling path emits a *colour* out of a uniform green backdrop, so bending the direction
// it samples along leaves the readback green.
//
// Six branches, in the order a real consumer takes them: an unbound centre (w == 0) returns a
// bright magenta fallback — the only branch that may run while the handle slots hold an unpopulated
// sentinel, which is the whole reason the flag exists; Display.x set returns the published centre as
// radiance, so a frame reads back the position the drive said the probe sat at; Display.y and
// Display.z return the published frame's imaginary and real parts, a quaternion being four numbers
// where a frame carries three channels; Display.w set returns the corrected direction for the
// authored ray and proxy; otherwise it samples the capture through the same correction, the
// ordinary consuming path.
//
// The corrected direction is returned **encoded** as dir * 0.5 + 0.5. The terminal tonemap clamps
// its input to [0, 1] under every tone curve including None, so a signed unit vector reaches a
// readback only by being mapped into that range first; the normalize before it makes the encoded
// value independent of how far along the ray the hit landed, which is what lets a case assert an
// analytically-known direction rather than a magnitude the proxy's size decides.
struct MaterialParams
{
    float4 Center;      // engine-published: xyz the probe's world position, w 1 while a capture is bound
    float4 Orientation; // engine-published: the capture's frame as a quaternion, xyz then w
    float4 Display;     // selects what the fragment shows: x the centre, y the frame's xyz, z its w, w the correction
    float4 ProxyRay;    // xyz the ray direction, in the capture's frame; w > 0.5 selects the box proxy over the sphere
    float4 ProxyOrigin; // xyz the ray origin, probe-relative and in that frame; w the sphere proxy's radius
    float4 ProxyMin;    // xyz the box proxy's lower corner, probe-relative and in that frame
    float4 ProxyMax;    // xyz the box proxy's upper corner, in the same terms
    uint  Texture;
    uint  Sampler;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// The corrected direction for the authored ray against the authored proxy, sphere or box.
float3 CorrectedDirection(MaterialParams p, float3 dir)
{
    if (p.ProxyRay.w > 0.5)
    {
        return ProbeProxyBoxDirection(p.ProxyOrigin.xyz, dir, p.ProxyMin.xyz, p.ProxyMax.xyz);
    }
    return ProbeProxySphereDirection(p.ProxyOrigin.xyz, dir, p.ProxyOrigin.w);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

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

    if (p.Display.w > 0.5)
    {
        const float3 corrected = CorrectedDirection(p, p.ProxyRay.xyz);
        return float4(normalize(corrected) * 0.5 + 0.5, 1.0);
    }

    // The cooked proxy is degenerate (a zero radius and a zero-extent box), so the correction is
    // disabled and this samples the fixed direction unchanged: a material authoring no proxy shows
    // exactly what it showed before it included the helper.
    const float2 uv = OctahedralUV(normalize(CorrectedDirection(p, float3(0.0, -1.0, 0.0))));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
