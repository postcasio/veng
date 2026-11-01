#include "Veng/surface.slang"

// A light-channel Surface fixture (gpu): a diffuse surface in BaseColor whose surface flags are
// chosen by Mode.
//   0 — the light channels Channels names (SurfaceLightChannels), no other flag;
//   1 — one of the 128 combinations of the three lighting flags, the three channel bits and the
//       metering bit per cell of a 16×8 grid over the screen, cell i holding the code i spreads
//       over bits 0-5 and 7, built from the flag constants;
//   2 — RawFlags written as the channel's value.
struct MaterialParams
{
    float4 BaseColor;
    float  Channels;
    float  Mode;
    float  RawFlags;
};

// The flags value of grid cell i: bits 0 and 1 the lighting flags, bits 2-4 the channel bits (bit 2
// leaving channel 0, bits 3 and 4 joining channels 1 and 2), bit 5 the ambient-diffuse flag, bit 6
// of i the metering bit.
float GridFlags(uint i)
{
    const uint mask = ((i & 4u) == 0u ? 0x1u : 0u) | ((i & 8u) != 0u ? 0x2u : 0u) |
                      ((i & 16u) != 0u ? 0x4u : 0u);
    return ((i & 1u) != 0u ? SurfaceFlagNoShadowReception : 0.0) +
           ((i & 2u) != 0u ? SurfaceFlagNoEnvironmentLighting : 0.0) + SurfaceLightChannels(mask) +
           ((i & 32u) != 0u ? SurfaceFlagNoAmbientDiffuse : 0.0) +
           ((i & 64u) != 0u ? SurfaceFlagExcludedFromMetering : 0.0);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    const ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float flags = SurfaceLightChannels(uint(p.Channels));
    if (p.Mode > 1.5)
    {
        flags = p.RawFlags;
    }
    else if (p.Mode > 0.5)
    {
        const uint2 cell = min(uint2(input.sv_position.xy / vc.ExtentParams.xy * float2(16.0, 8.0)),
                               uint2(15u, 7u));
        flags = GridFlags(cell.x + 16u * cell.y);
    }

    GBufferOutput output;
    output.Albedo = float4(p.BaseColor.rgb, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, flags);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
