#include "Veng/forward_lighting.slang"

// Forward-lit Translucent fixture (gpu): shades its authored parameters through the engine's
// forward light loop and returns the lit radiance at its authored coverage — the translucent twin
// of forward_opaque.frag.
struct MaterialParams
{
    float4 BaseColor; // linear rgb; a unused
    float  Roughness;
    float  Metallic;
    float  Occlusion;
    float  Coverage;
};

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);

    ForwardSurface surface;
    surface.Position = ForwardWorldPosition(input.sv_position);
    surface.Normal = normalize(input.v_WorldNormal);
    surface.View = ForwardViewVector(surface.Position);
    surface.Albedo = p.BaseColor.rgb;
    surface.Roughness = p.Roughness;
    surface.Metallic = p.Metallic;
    surface.Occlusion = p.Occlusion;

    // Two guarded components can sum past the guard's bound, so the sum takes the guard again.
    ForwardLighting lit = EvaluateForwardLighting(surface);
    return float4(GuardLightingOutput(lit.Diffuse + lit.Specular), p.Coverage);
}
