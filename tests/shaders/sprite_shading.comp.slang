// Evaluates the flipbook shading model's binding-free core (Veng/sprite_shading_core.slang) and the
// view-depth solve beside the opaque-depth readers (Veng/scene_depth.slang) on cases the test
// uploads, so a test can pin each function's properties without a scene, a light or a rendered
// image. The core reads no binding of its own, so the buffers, textures and sampler here are the
// only ones bound; set 0 is reserved for the bindless registry, so they bind on set 3 as the other
// test shaders do. One dispatch evaluates one mode over its cases, one invocation per case.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

#include "Veng/scene_depth.slang"
#include "Veng/sprite_shading_core.slang"

[[vk::binding(0, 3)]] ByteAddressBuffer u_Cases;                       // per-mode float4 cases
[[vk::binding(1, 3)]] RWStructuredBuffer<float4> u_Results;            // per-mode float4 results
[[vk::binding(2, 3)]] Texture2D u_Atlas;                               // two frames, side by side
[[vk::binding(3, 3)]] Texture2D u_Motion;                              // their motion sheet
[[vk::binding(4, 3)]] SamplerState u_Sampler;                          // linear, clamped
[[vk::binding(5, 3)]] StructuredBuffer<SpriteEmissionRamp> u_Ramp;     // one ramp

// The modes, matching the test's.
static const uint ModeWeights = 0;     // case xyz a unit direction -> (wA, sum), (wB, 0)
static const uint ModeTemperature = 1; // case x stored -> (sqrt decode, linear decode, 0, 0)
static const uint ModeRamp = 2;        // case x t, y gate width -> (ramp rgb, gate)
static const uint ModeFrameBlend = 3;  // case xy quad UV, z motion scale (0 none) -> blended texel
static const uint ModeSoftFade = 4;    // case x gap, y distance -> (fade, 0, 0, 0)
static const uint ModeViewDepth = 5;   // cases (A, B, C, D), (device depth) -> (view depth)

struct SpriteShadingPush
{
    uint Mode;
    uint CaseCount;
    float Fraction;
    uint Pad;
};
[[vk::push_constant]] SpriteShadingPush u_Push;

float4 LoadCase(uint index)
{
    return u_Cases.Load<float4>(index * 16);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= u_Push.CaseCount)
    {
        return;
    }

    if (u_Push.Mode == ModeWeights)
    {
        float3 wA;
        float3 wB;
        SixWayWeights(LoadCase(i).xyz, wA, wB);
        u_Results[2 * i] = float4(wA, dot(wA, float3(1.0)) + dot(wB, float3(1.0)));
        u_Results[2 * i + 1] = float4(wB, 0.0);
    }
    else if (u_Push.Mode == ModeTemperature)
    {
        float stored = LoadCase(i).x;
        u_Results[i] = float4(DecodeTemperature(stored, true), DecodeTemperature(stored, false),
                              0.0, 0.0);
    }
    else if (u_Push.Mode == ModeRamp)
    {
        float4 c = LoadCase(i);
        SpriteEmissionRamp ramp = u_Ramp[0];
        u_Results[i] = float4(EvaluateEmissionRamp(ramp, c.x), IgnitionGate(c.x, ramp.Params.x, c.y));
    }
    else if (u_Push.Mode == ModeFrameBlend)
    {
        float4 c = LoadCase(i);
        SpriteFramePair pair;
        pair.RectN = float4(1.0 / 64.0, 1.0 / 32.0, 0.5 - 1.0 / 64.0, 1.0 - 1.0 / 32.0);
        pair.RectN1 = float4(0.5 + 1.0 / 64.0, 1.0 / 32.0, 1.0 - 1.0 / 64.0, 1.0 - 1.0 / 32.0);
        pair.TileSize = float2(0.5, 1.0);
        pair.ScaleN = 1.0;
        pair.ScaleN1 = 1.0;
        pair.Fraction = u_Push.Fraction;
        SpriteFrameTaps taps = SpriteFrameTapsAt(pair, c.xy, float2(0.0), float2(0.0),
                                                 float2(0.0), float2(0.0));
        if (c.z > 0.0)
        {
            float2 storedN = u_Motion.SampleLevel(u_Sampler, taps.UvN, 0.0).rg;
            float2 storedN1 = u_Motion.SampleLevel(u_Sampler, taps.UvN1, 0.0).rg;
            taps = SpriteFrameTapsAt(pair, c.xy, float2(0.0), float2(0.0),
                                     DecodeMotion(storedN, c.z), DecodeMotion(storedN1, c.z));
        }
        u_Results[i] = SampleFrameBlended(u_Atlas, u_Sampler, taps);
    }
    else if (u_Push.Mode == ModeSoftFade)
    {
        float4 c = LoadCase(i);
        u_Results[i] = float4(SoftEdgeFade(c.x, c.y), 0.0, 0.0, 0.0);
    }
    else if (u_Push.Mode == ModeViewDepth)
    {
        float4 row = LoadCase(2 * i);
        float4x4 proj = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0),
                                 float4(0.0, 0.0, row.x, row.y), float4(0.0, 0.0, row.z, row.w));
        u_Results[i] = float4(ViewDepthFromProjection(proj, LoadCase(2 * i + 1).x), 0.0, 0.0, 0.0);
    }
}
