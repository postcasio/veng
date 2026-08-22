#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"
#include "Veng/probeproxy.slang"

// A diegetic reflective panel material: reflects the eye ray off the panel's surface and samples the
// CaptureSurface's octahedral scene capture along it, so the panel shows the scene mirrored about
// its own plane and the image slides across it as the viewer moves — which is what a mirror does,
// and what sampling by the surface normal alone cannot do (that shows one fixed image from every
// angle). CaptureSurface binds the capture's octahedral output onto the Texture/Sampler handle
// fields each frame (a runtime bindless slot, not a cooked id); the fragment returns the sampled
// pre-tonemap HDR color as the surface's radiance, so a bright captured pixel blooms through the
// scene's own bloom.
//
// The capture is a view from one point, so a bare reflected ray would show the scene as though
// everything in it were infinitely far away — visibly wrong here, where the reflected content stands
// a couple of metres off. ProxyRadius authors a sphere standing in for that content, and
// Veng/probeproxy.slang re-takes the direction from the probe centre to where the ray meets it. The
// component's Center Slot publishes that centre into ProbeCenter, whose w is the validity flag this
// fragment gates on before it indexes the bindless slots at all.
//
// The proxy is expressed in the capture's own frame, and this capture is World-aligned — the
// default — so its frame is world space and the ray needs no conversion. A capture oriented in a
// moving carrier's frame would need the Orientation Slot and a rotation into it before the call.
struct MaterialParams
{
    float4 ProbeCenter; // engine-published: xyz the probe's world position, w 1 while a capture is bound
    float  ProxyRadius; // radius of the sphere standing in for the reflected scene, about the probe
    uint   Texture;
    uint   Sampler;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // No capture bound: the handle slots hold the bindless sentinel before the component's first
    // drive fills them and again once its teardown clears them, so this gate is what keeps the
    // fragment from indexing the texture array with it. A dark panel reads as unpolished glass.
    if (p.ProbeCenter.w <= 0.5)
    {
        return float4(0.02, 0.02, 0.025, 1.0);
    }

    // The fragment's own world position, reconstructed from its window depth: the eye ray needs a
    // point on the panel, and SurfaceFragmentInput carries no world-position interpolant.
    const float2 ndc = SceneScreenUV(vc, input.sv_position) * 2.0 - 1.0;
    const float4 worldH = mul(vc.InvViewProj, float4(ndc, input.sv_position.z, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;

    const float3 eyeRay = normalize(worldPos - vc.CameraPosition.xyz);
    const float3 reflected = reflect(eyeRay, normalize(input.v_WorldNormal));

    const float3 corrected =
        ProbeProxySphereDirection(worldPos - p.ProbeCenter.xyz, reflected, p.ProxyRadius);

    const float2 uv = OctahedralUV(normalize(corrected));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
