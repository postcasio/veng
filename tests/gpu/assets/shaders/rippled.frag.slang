#include "Veng/surface.slang"

// Deferred opaque fixture fragment stage (gpu) whose shading normal ripples: the brick fixture's
// g-buffer writes, with the interpolated normal tilted by a fine sinusoid along both UV axes, so a
// surface carries normal variation finer than a few pixels for the specular anti-aliasing cases. The
// tilt is applied in world X and Z, so it is a ripple across a +Y-facing plane.
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;
    uint   ORM;              uint ORMSampler;
    float4 BaseColorFactor;
    float4 EmissiveFactor;
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float SurfaceFlags;
    float  RippleFrequency;  // cycles across the UV unit square, per axis
    float  RippleAmplitude;  // peak tilt, as the tangent of the angle
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    nointerpolation uint v_MaterialOffset : TEXCOORD1;
    float4 v_CurClip : TEXCOORD2;
    float4 v_PrevClip : TEXCOORD3;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);

    float4 baseColor = g_Textures[NonUniformResourceIndex(p.BaseColor)].Sample(
        g_Samplers[NonUniformResourceIndex(p.BaseColorSampler)], input.v_UV);

    float2 phase = 2.0 * 3.14159265 * p.RippleFrequency * input.v_UV;
    float2 tilt = p.RippleAmplitude * sin(phase);
    float3 normal = normalize(normalize(input.v_WorldNormal) + float3(tilt.x, 0.0, tilt.y));

    GBufferOutput output;
    output.Albedo = baseColor * p.BaseColorFactor;
    output.Normal = float4(normal, 0.0);
    output.ORM = float4(1.0, p.RoughnessFactor, p.MetallicFactor, p.SurfaceFlags);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
