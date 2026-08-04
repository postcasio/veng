// Volume field: ray-marches a bounded emissive, light-absorbing medium (a 3D emission+extinction
// texture with world-space bounds) along each pixel's view ray and composites the result into the
// lit scene color. Occupies the scene-color slot after deferred lighting + the sky composite and
// before the refraction grab / translucent pass, blended (srcColor = ONE, dstColor = SRC_ALPHA):
// the fragment returns float4(accumulated emission, surviving transmittance), so one blend both adds
// the medium's glow and attenuates the background behind it.
//
// The g-buffer depth is sampled through set-0 bindless; the field's 3D texture + sampler ride a
// dedicated per-pass set (set 1) — a Texture3D in set 0's Metal argument buffer is a MoltenVK risk
// the engine refuses. Bounds, the Opacity-folded scales, the step count, and the frame index for
// the jitter rotation ride the push.
#include "../view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

[[vk::binding(0, 1)]] Texture3D<float4> g_Volume;
[[vk::binding(1, 1)]] SamplerState g_VolumeSampler;

struct PushConstants
{
    float4 BoundsMin;        // xyz world-space AABB min; w unused
    float4 BoundsMax;        // xyz world-space AABB max; w unused
    float4 Scales;           // x emission factor (EmissionScale·Opacity), y extinction factor, zw unused
    uint DepthTexture;       // g-buffer depth bindless index
    uint Sampler;            // shared sampler bindless index
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint Steps;              // fixed march step count
    uint FrameIndex;         // monotonic frame counter for the temporal jitter rotation
    uint Pad0;
    uint Pad1;
    uint Pad2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Interleaved gradient noise (Jiménez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare", SIGGRAPH 2014) — a cheap per-pixel [0,1) dither. The frame index rotates the
// pattern by the golden-ratio conjugate so the per-pixel start offset decorrelates over time and the
// TAA resolve integrates the march jitter into a smooth result.
float InterleavedGradientNoise(float2 pixel, uint frame)
{
    pixel += float(frame) * 5.588238f; // 2·golden-ratio-conjugate·... : an irrational per-frame shift
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// Reconstructs the world position of a pixel at NDC depth d through the inverse view-projection.
float3 WorldFromDepth(ViewConstants vc, float2 ndcXY, float d)
{
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, d, 1.0));
    return worldH.xyz / worldH.w;
}

// Maps a world position into the field's [0,1]^3 texture space from its world-space bounds.
float3 VolumeUvw(float3 worldPos)
{
    return (worldPos - g_PC.BoundsMin.xyz) / (g_PC.BoundsMax.xyz - g_PC.BoundsMin.xyz);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float2 sampleUV = ScaledSampleUV(input.v_UV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    // The view ray from the camera through this pixel, and the world position of the nearest opaque
    // surface (reverse-Z: depth == 0 is the cleared background — the far plane). The ray direction
    // forms homogeneously — dividing by the far-plane w would NaN every pixel at extreme Far/Near,
    // where it collapses to zero in f32 (see Veng/sky.slang SkyViewDirection); the chosen NDC z is
    // immaterial to the normalized direction.
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float4 farH = mul(vc.InvViewProj, float4(ndcXY, 1.0, 1.0));
    float3 origin = vc.CameraPosition.xyz;
    float3 dir = normalize(farH.xyz - origin * farH.w);
    float sceneDistance = (depth > 0.0) ? length(WorldFromDepth(vc, ndcXY, depth) - origin) : 1e30;

    // Slab intersection of the ray with the field's AABB (mirrors Renderer::ComputeMarchSegment).
    float3 invDir = 1.0 / dir;
    float3 t0 = (g_PC.BoundsMin.xyz - origin) * invDir;
    float3 t1 = (g_PC.BoundsMax.xyz - origin) * invDir;
    float3 tSmall = min(t0, t1);
    float3 tLarge = max(t0, t1);
    float tEnter = max(max(tSmall.x, tSmall.y), tSmall.z);
    float tExit = min(min(tLarge.x, tLarge.y), tLarge.z);

    // Clamp entry to the camera and exit to the nearest opaque surface; discard an empty segment
    // (missed the box, grazing, or geometry occludes the field entirely).
    tEnter = max(tEnter, 0.0);
    tExit = min(tExit, sceneDistance);
    if (tExit <= tEnter)
    {
        discard;
    }

    uint steps = max(g_PC.Steps, 1u);
    float segment = tExit - tEnter;
    float dt = segment / float(steps);

    // Per-pixel start jitter over one step, temporally rotated so TAA integrates it.
    float jitter = InterleavedGradientNoise(input.sv_position.xy, g_PC.FrameIndex);

    float emissionFactor = g_PC.Scales.x;
    float extinctionFactor = g_PC.Scales.y;

    // Front-to-back emission/absorption integration: at each step accumulate the emitted radiance
    // weighted by the surviving transmittance, then attenuate the transmittance by this step's
    // optical depth. Early-out once the medium is effectively opaque.
    float3 L = float3(0.0);
    float T = 1.0;
    for (uint i = 0; i < steps; ++i)
    {
        float t = tEnter + (float(i) + jitter) * dt;
        float3 uvw = VolumeUvw(origin + dir * t);
        float4 sample = g_Volume.SampleLevel(g_VolumeSampler, uvw, 0.0);

        float3 emission = sample.rgb * emissionFactor;
        float extinction = sample.a * extinctionFactor;

        L += T * emission * dt;
        T *= exp(-extinction * dt);
        if (T < 0.003)
        {
            T = 0.0;
            break;
        }
    }

    // src RGB = accumulated emission, src A = surviving transmittance; the (ONE, SRC_ALPHA) blend
    // adds L and multiplies the background by T.
    return float4(L, T);
}
