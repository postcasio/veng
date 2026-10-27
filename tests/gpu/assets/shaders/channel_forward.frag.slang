#include "Veng/forward_lighting.slang"

// The forward-lit twin of channel_surface.frag (gpu fixture): a diffuse surface in BaseColor in
// the light channels Channels names, at full coverage.
struct MaterialParams
{
    float4 BaseColor;
    float  Channels;
};

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);

    ForwardSurface surface;
    surface.Position = ForwardWorldPosition(input.sv_position);
    surface.Normal = normalize(input.v_WorldNormal);
    surface.View = ForwardViewVector(surface.Position);
    surface.Albedo = p.BaseColor.rgb;
    surface.Roughness = 1.0;
    surface.LightChannels = uint(p.Channels);

    const ForwardLighting lit = EvaluateForwardLighting(surface);
    return float4(GuardLightingOutput(lit.Diffuse + lit.Specular), 1.0);
}
