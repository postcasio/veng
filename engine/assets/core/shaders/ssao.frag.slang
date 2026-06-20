// Screen-space ambient occlusion fragment stage: samples the g-buffer depth and
// world-space normal (G1) through the bindless set-0 array, reconstructs the
// view-space position from depth, transforms the normal into view space, and
// estimates occlusion with a fixed 16-sample hemisphere kernel rotated by a tiling
// noise vector. Writes a single-channel occlusion factor (1 = unoccluded) to an
// R8 target the lighting pass folds into its ambient term.
//
// View space is the standard, well-behaved domain for the hemisphere kernel and
// its range check. The view/projection matrices ride the per-frame view-constants
// buffer (set-0 binding 5), the same buffer the lighting pass reads; radius,
// intensity, and bias are fixed kernel constants.
// The canonical per-frame view-constants block (set 0, binding 5) and its stride
// are declared in view_constants.slang — the single source shared with the lighting
// and material shaders, so the View/Proj this pass reads land at the exact byte
// offsets the renderer writes (and at the correct per-frame region).
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint NormalTexture;       // G1 world-space normal bindless index
    uint DepthTexture;        // depth bindless index
    uint Sampler;             // shared sampler index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint2 Extent;             // the AO target's pixel dimensions (for noise tiling)
    uint Pad0;
    uint Pad1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Fixed SSAO kernel constants. A larger radius spreads occlusion further; bias
// rejects self-occlusion at grazing angles; intensity scales the final darkening.
static const float Radius = 0.5;
static const float Bias = 0.025;
static const float Intensity = 1.5;
static const uint KernelSize = 16;

// A fixed 16-sample hemisphere kernel (z >= 0, weighted toward the origin) — a
// reproducible kernel so the golden's AO noise is deterministic run to run.
static const float3 Kernel[16] = {
    float3( 0.0490,  0.0333,  0.0263),
    float3(-0.0617,  0.0814,  0.0668),
    float3( 0.0214, -0.1119,  0.0921),
    float3(-0.1373, -0.0731,  0.1166),
    float3( 0.1568,  0.1093,  0.0479),
    float3( 0.1019, -0.1684,  0.1531),
    float3(-0.2150,  0.0497,  0.1043),
    float3(-0.0588,  0.2433,  0.0875),
    float3( 0.2611,  0.1019,  0.1869),
    float3(-0.1429, -0.2855,  0.0617),
    float3(-0.3138,  0.1657,  0.2284),
    float3( 0.2199, -0.2199,  0.3286),
    float3( 0.0734,  0.4090,  0.1632),
    float3(-0.4456, -0.1283,  0.2659),
    float3( 0.3680, -0.3680,  0.1571),
    float3( 0.0860,  0.0860,  0.6204),
};

// A general 4x4 inverse. The view-constants buffer carries Proj, not its inverse,
// so the depth reconstruction inverts it here. The projection is well-conditioned,
// so this is numerically safe.
float4x4 Inverse4x4(float4x4 m)
{
    float4x4 inv;
    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];

    float b00 = a00 * a11 - a01 * a10;
    float b01 = a00 * a12 - a02 * a10;
    float b02 = a00 * a13 - a03 * a10;
    float b03 = a01 * a12 - a02 * a11;
    float b04 = a01 * a13 - a03 * a11;
    float b05 = a02 * a13 - a03 * a12;
    float b06 = a20 * a31 - a21 * a30;
    float b07 = a20 * a32 - a22 * a30;
    float b08 = a20 * a33 - a23 * a30;
    float b09 = a21 * a32 - a22 * a31;
    float b10 = a21 * a33 - a23 * a31;
    float b11 = a22 * a33 - a23 * a32;

    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    float invDet = 1.0 / det;

    inv[0][0] = ( a11 * b11 - a12 * b10 + a13 * b09) * invDet;
    inv[0][1] = (-a01 * b11 + a02 * b10 - a03 * b09) * invDet;
    inv[0][2] = ( a31 * b05 - a32 * b04 + a33 * b03) * invDet;
    inv[0][3] = (-a21 * b05 + a22 * b04 - a23 * b03) * invDet;
    inv[1][0] = (-a10 * b11 + a12 * b08 - a13 * b07) * invDet;
    inv[1][1] = ( a00 * b11 - a02 * b08 + a03 * b07) * invDet;
    inv[1][2] = (-a30 * b05 + a32 * b02 - a33 * b01) * invDet;
    inv[1][3] = ( a20 * b05 - a22 * b02 + a23 * b01) * invDet;
    inv[2][0] = ( a10 * b10 - a11 * b08 + a13 * b06) * invDet;
    inv[2][1] = (-a00 * b10 + a01 * b08 - a03 * b06) * invDet;
    inv[2][2] = ( a30 * b04 - a31 * b02 + a33 * b00) * invDet;
    inv[2][3] = (-a20 * b04 + a21 * b02 - a23 * b00) * invDet;
    inv[3][0] = (-a10 * b09 + a11 * b07 - a12 * b06) * invDet;
    inv[3][1] = ( a00 * b09 - a01 * b07 + a02 * b06) * invDet;
    inv[3][2] = (-a30 * b03 + a31 * b01 - a32 * b00) * invDet;
    inv[3][3] = ( a20 * b03 - a21 * b01 + a22 * b00) * invDet;
    return inv;
}

// Reconstruct the view-space position of a screen UV + sampled depth: form NDC,
// transform by the precomputed inverse projection, and perspective-divide.
float3 ViewPositionFromDepth(float4x4 invProj, float2 uv, float depth)
{
    float2 ndcXY = uv * 2.0 - 1.0;
    float4 clip = float4(ndcXY, depth, 1.0);
    float4 viewH = mul(invProj, clip);
    return viewH.xyz / viewH.w;
}

// A small tiling noise rotation: a per-pixel pseudo-random 2D vector in [-1, 1]
// from the integer pixel coordinate, tiled every 4 pixels so neighboring kernels
// rotate differently and the raw occlusion decorrelates without a noise texture.
float3 NoiseRotation(uint2 pixel)
{
    uint2 t = pixel & 3u;
    float n = frac(sin(dot(float2(t), float2(12.9898, 78.233))) * 43758.5453);
    float angle = n * 6.2831853;
    return float3(cos(angle), sin(angle), 0.0);
}

[shader("fragment")]
float fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];

    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, input.v_UV).r;

    // The cleared depth (1.0) marks the background, where there is no geometry to
    // occlude — leave it fully unoccluded so the lighting pass's background ambient
    // is unchanged.
    if (depth >= 1.0)
        return 1.0;

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4x4 invProj = Inverse4x4(vc.Proj);
    float3 viewPos = ViewPositionFromDepth(invProj, input.v_UV, depth);

    // The world-space normal (G1) carried into view space by the view rotation.
    float3 worldNormal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, input.v_UV).xyz;
    float normalLength = length(worldNormal);
    if (normalLength < 1e-4)
        return 1.0;
    float3 N = mul((float3x3)vc.View, worldNormal / normalLength);

    // Build a TBN basis from the noise-rotated tangent (Gram-Schmidt) so the
    // hemisphere kernel orients around the surface normal with per-pixel rotation.
    uint2 pixel = uint2(input.v_UV * float2(g_PC.Extent));
    float3 randomVec = NoiseRotation(pixel);
    float3 tangent = normalize(randomVec - N * dot(randomVec, N));
    float3 bitangent = cross(N, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, N);

    float occlusion = 0.0;
    for (uint i = 0; i < KernelSize; ++i)
    {
        // Place the sample in view space around the fragment, oriented by TBN.
        float3 samplePos = viewPos + mul(Kernel[i], TBN) * Radius;

        // Project the sample to screen UV to look up the stored depth there.
        float4 offset = mul(vc.Proj, float4(samplePos, 1.0));
        offset.xyz /= offset.w;
        float2 sampleUV = offset.xy * 0.5 + 0.5;

        // Sample the stored depth at that UV and reconstruct its view-space Z.
        float sampleDepth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;
        float3 storedPos = ViewPositionFromDepth(invProj, sampleUV, sampleDepth);

        // A range check fades the contribution when the occluder is far in depth, so
        // a distant surface behind the sample does not over-darken.
        float rangeCheck = smoothstep(0.0, 1.0, Radius / max(abs(viewPos.z - storedPos.z), 1e-4));

        // The stored geometry occludes the sample when it sits in front of it (a
        // larger, less-negative view Z in the right-handed view space) past the bias.
        occlusion += (storedPos.z >= samplePos.z + Bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(KernelSize)) * Intensity;
    return saturate(occlusion);
}
