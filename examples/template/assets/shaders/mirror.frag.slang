#include "Veng/translucent.slang"
#include "Veng/probedepth.slang"

// A diegetic reflective panel material: it shows the scene reflected about its own surface. A mirror
// reflects the eye ray off the surface normal — sampling the CaptureSurface's octahedral scene
// capture by that reflected direction — so the image on the panel changes with the viewer's angle,
// which sampling by the panel's own normal (one image from every angle) does not. And the reflected
// scene is at a finite distance, so the reflected ray is parallax-corrected against the capture's
// recorded distance map (Veng/probedepth.slang) before it samples the radiance map: this places a
// reflected surface where it actually is rather than at infinity.
//
// CaptureSurface binds the capture's octahedral radiance output onto the Texture/Sampler slots, its
// distance map onto the Depth/DepthSampler slots, and the probe centre (with a validity flag in its
// w) onto Center, all as runtime bindless slots rebound each frame — not cooked ids. The fragment
// gates on that validity flag before indexing the bindless slots: cleared, the handles hold the
// unbound sentinel, and the fragment falls back to the bare reflection direction. It returns the
// sampled pre-tonemap HDR colour as the surface's radiance, so a bright captured pixel blooms.
struct MaterialParams
{
    float4 Center;      // engine-bound: xyz the probe's world centre, w 1 while a capture is bound
    uint   Texture;     // engine-bound: the octahedral radiance map
    uint   Sampler;     // engine-bound: the radiance map's clamp sampler
    uint   Depth;       // engine-bound: the octahedral distance map
    uint   DepthSampler; // engine-bound: the distance map's point clamp sampler
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    // Reconstruct this fragment's world position from its window depth (the reconstruction deferred
    // lighting and ComputeMotionVector share), then reflect the eye ray to it off the surface normal.
    const ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float2 ndc = SceneScreenUV(vc, input.sv_position) * 2.0 - 1.0;
    const float4 worldH = mul(vc.InvViewProj, float4(ndc, input.sv_position.z, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;

    const float3 normal = normalize(input.v_WorldNormal);
    const float3 eye = normalize(worldPos - vc.CameraPosition.xyz);
    const float3 reflectionDir = reflect(eye, normal);

    // Gate on the published validity flag before indexing the runtime-bound slots. The capture is
    // World-aligned, so the reflected world-space ray is already in the map's frame; correct it
    // against the recorded distance when bound, else sample by the bare direction (the probe treated
    // as infinitely distant — the unbound handles would otherwise index the bindless array with a
    // sentinel).
    float3 sampleDir = reflectionDir;
    if (p.Center.w > 0.5)
    {
        sampleDir = CaptureDepthCorrect(worldPos - p.Center.xyz, reflectionDir,
                                        g_Textures[NonUniformResourceIndex(p.Depth)],
                                        g_Samplers[NonUniformResourceIndex(p.DepthSampler)]);
    }

    const float2 uv = OctahedralUV(sampleDir);
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
