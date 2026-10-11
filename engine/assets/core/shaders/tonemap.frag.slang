// Tonemap fragment stage: the engine's first PostProcess-domain material. It samples the HDR
// scene colour through the bindless set-0 array (the HDR input is a runtime-bound handle field
// the post pass rewrites each frame), adds the accumulated bloom when bloom is active and the
// lens dirt that bloom lights when a dirt mask is set, applies an
// exposure scale, and maps the linear HDR color to the display range with a selectable tone curve
// (the Tonemapper param, written each frame from the SceneView). The terminal stage of the deferred
// chain — it replaces a plain clamp with a curve that rolls off highlights instead of hard-clipping
// them.
//
// The output stays linear; the swapchain composite handles the final display (sRGB) encoding, so
// every operator below returns linear color and any curve authored against a display gamma undoes
// that gamma before returning (AgX).

// The PostProcess contract: the set-0 bindless declarations, the post push block (g_PC), and the
// PostProcessFragmentInput struct this fragment reads.
#include "Veng/postprocess.slang"

// MaterialParams is this material's whole parameter block: the bindless handle slots for the HDR
// and bloom inputs (uint members written each frame with the renderer's current target indices)
// beside the authored Exposure and Tonemapper params. Read through the standard set-0
// byte-addressed material buffer at this material's own block offset.
struct MaterialParams
{
    // Field order carries no correctness weight: the cooker reflects the block in the same
    // scalar/tight layout the shader's Load reads, so a vector after a scalar packs identically
    // on both sides.
    float4 RenderScale; // dynamic-resolution upscale: xy source validExtent/allocExtent, zw clamp
    uint   Hdr;         // bindless sampled-image index (runtime-bound each frame)
    uint   HdrSampler;  // bindless sampler index (runtime-bound each frame)
    float4 BloomScale;  // bloom mip-0 sub-rect map: xy validExtent/allocExtent, zw clamp
    uint   Bloom;       // bindless index of bloom pyramid mip 0 (runtime-bound each frame)
    uint   BloomSampler; // bindless sampler index for the bloom read (runtime-bound each frame)
    float  BloomIntensity; // bloom mix, set per frame; 0 when bloom is not active
    float  Exposure;    // authored param, set per frame from the SceneView
    float  Tonemapper;  // the Renderer::Tonemapper enum as a float, set per frame from the SceneView
    float  OutputBrightness; // display-calibration multiply, set per frame from the SceneView
    float  OutputGamma;      // display-calibration gamma, set per frame from the SceneView
    float4 LensDirtCover;    // cover fit of the dirt mask over the frame: xy scale, zw offset
    float4 LensDirtBloomScale; // the dirt term's bloom level sub-rect map: xy scale, zw clamp
    float4 LensDirtBloomTexel; // that level's extent: xy texels, zw their reciprocal
    uint   LensDirt;         // bindless index of the dirt mask (written each frame)
    uint   LensDirtSampler;  // bindless index of the mask's own sampler (written each frame)
    uint   LensDirtBloom;    // bindless index of the dirt term's bloom level (runtime-bound)
    uint   LensDirtBloomSampler; // bindless sampler for that level (runtime-bound)
    float  LensDirtIntensity; // the dirt term's mix, set per frame; 0 when no dirt is drawn
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

// The Narkowicz ACES filmic approximation: a rational fit of the ACES reference tone curve, applied
// per channel. Lifts the low-mids and desaturates highlights hard.
float3 TonemapAces(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// The Reinhard operator x/(1+x), per channel: a simple, shoulderless rolloff.
float3 TonemapReinhard(float3 x)
{
    return x / (1.0 + x);
}

// The Khronos PBR Neutral tone mapper (glTF-Sample-Renderer reference). Hue- and
// saturation-preserving: it subtracts a small dark-region offset (blacks stay black), compresses
// only the peak channel past a knee, and desaturates toward white solely in proportion to how far
// the peak was compressed — so a saturated colour keeps its hue until it is forced to the top.
float3 TonemapPbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }

    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// AgX (the widely-used minimal fit of Troy Sobotka's AgX display transform). Maps the exposed color
// through an inset matrix into a log2 range, applies a 6th-order sigmoid contrast, and an outset
// matrix — a neutral filmic curve whose highlight desaturation is gradual and hue-stable. The fit
// targets a 2.2-gamma display, so the tail undoes that gamma to return linear color.
static const float3x3 AgxInset = float3x3(
    0.8566271533, 0.1373189729, 0.1118982130,
    0.0951212405, 0.7612419906, 0.0767994186,
    0.0482516061, 0.1014390365, 0.8113023684);
static const float3x3 AgxOutset = float3x3(
    1.1271005818, -0.1413297635, -0.1413297635,
    -0.1106066431, 1.1578237022, -0.1106066431,
    -0.0164939387, -0.0164939387, 1.2519364066);
static const float AgxMinEv = -12.47393;
static const float AgxMaxEv = 4.026069;

float3 AgxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
           0.1191 * x - 0.00232;
}

float3 TonemapAgx(float3 color)
{
    color = mul(AgxInset, color);
    color = clamp(log2(max(color, 1e-10)), AgxMinEv, AgxMaxEv);
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    color = AgxContrast(color);
    color = mul(AgxOutset, color);
    // The fit's output is display-referred (2.2 gamma); undo it so the returned color is linear.
    return pow(saturate(color), 2.2.xxx);
}

// A cubic B-spline read of a coarse bloom level in four bilinear taps. The dirt term magnifies its
// level many times over, and a bilinear read draws a bright source's glow there as a visible
// diamond; the B-spline's smooth kernel draws it round.
float3 SampleBloomBSpline(uint texture, uint samplerIndex, float2 uv, float4 texel)
{
    const float2 p = uv * texel.xy - 0.5;
    const float2 i = floor(p);
    const float2 f = p - i;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    const float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    const float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    const float2 w3 = f3 / 6.0;
    const float2 g0 = w0 + w1;
    const float2 g1 = w2 + w3;
    // Each pair of taps folds into one bilinear read between them.
    const float2 uv0 = (i - 1.0 + w1 / g0 + 0.5) * texel.zw;
    const float2 uv1 = (i + 1.0 + w3 / g1 + 0.5) * texel.zw;
    Texture2D t = g_Textures[NonUniformResourceIndex(texture)];
    SamplerState s = g_Samplers[NonUniformResourceIndex(samplerIndex)];
    return g0.y * (g0.x * t.Sample(s, float2(uv0.x, uv0.y)).rgb +
                   g1.x * t.Sample(s, float2(uv1.x, uv0.y)).rgb) +
           g1.y * (g0.x * t.Sample(s, float2(uv0.x, uv1.y)).rgb +
                   g1.x * t.Sample(s, float2(uv1.x, uv1.y)).rgb);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialOffset);
    // The HDR source is rendered into a dynamic-resolution sub-rect; this terminal pass renders the
    // full output and upscales by mapping the full-output UV across the source's valid region
    // (RenderScale.xy), clamped (RenderScale.zw) so the bilinear tap never bleeds past it. At scale
    // 1.0 RenderScale is (1,1,~1,~1) and this is the plain full-resolution sample.
    float2 uv = min(input.v_UV * p.RenderScale.xy, p.RenderScale.zw);
    float3 hdr = g_Textures[NonUniformResourceIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformResourceIndex(p.HdrSampler)], uv).rgb;
    // Bloom is added in linear HDR ahead of exposure; its mip 0 is half the scene's resolution, so
    // this one bilinear tap magnifies it. The branch is uniform across the draw, and with bloom
    // inactive the Bloom slot is never indexed.
    if (p.BloomIntensity > 0.0)
    {
        const float2 bloomUv = min(input.v_UV * p.BloomScale.xy, p.BloomScale.zw);
        hdr += g_Textures[NonUniformResourceIndex(p.Bloom)].Sample(
                   g_Samplers[NonUniformResourceIndex(p.BloomSampler)], bloomUv).rgb *
               p.BloomIntensity;
    }
    // Lens dirt: the light a dirty lens scatters, the wide bloom times the mask. The mask is read
    // in output space through its cover fit, so it is fixed on screen; the branch is uniform, and
    // with no dirt neither slot is indexed.
    if (p.LensDirtIntensity > 0.0)
    {
        const float2 dirtUv = input.v_UV * p.LensDirtCover.xy + p.LensDirtCover.zw;
        const float dirt = g_Textures[NonUniformResourceIndex(p.LensDirt)].Sample(
                               g_Samplers[NonUniformResourceIndex(p.LensDirtSampler)], dirtUv).r;
        const float2 wideUv = min(input.v_UV * p.LensDirtBloomScale.xy, p.LensDirtBloomScale.zw);
        const float3 wide = SampleBloomBSpline(p.LensDirtBloom, p.LensDirtBloomSampler, wideUv,
                                               p.LensDirtBloomTexel);
        hdr += wide * (dirt * p.LensDirtIntensity);
    }
    float3 exposed = max(hdr * p.Exposure, 0.0);

    // Branch on the selected operator (the Renderer::Tonemapper enum carried as a float).
    const uint op = uint(p.Tonemapper + 0.5);
    float3 tonemapped;
    switch (op)
    {
    case 0: // None
        tonemapped = saturate(exposed);
        break;
    case 1: // Reinhard
        tonemapped = TonemapReinhard(exposed);
        break;
    case 2: // ACES
        tonemapped = TonemapAces(exposed);
        break;
    case 3: // AgX
        tonemapped = TonemapAgx(exposed);
        break;
    default: // PBRNeutral
        tonemapped = TonemapPbrNeutral(exposed);
        break;
    }

    // Display-calibration output knobs. Applied to the tonemapped (still-linear) color before the
    // swapchain composite's display encode: a brightness multiply then a gamma power. The neutral
    // pair (1, 1) skips the step, so the default output is bit-identical to before this existed.
    if (p.OutputBrightness != 1.0 || p.OutputGamma != 1.0)
    {
        tonemapped = pow(max(tonemapped * p.OutputBrightness, 0.0), (1.0 / p.OutputGamma).xxx);
    }
    return float4(tonemapped, 1.0);
}
